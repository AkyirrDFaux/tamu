// Host-side tests for bus retransmission (card t_763b4cde): the repeat guard and the retry backoff.
//
// Two defects, both in the retransmission path:
//
//   1. A re-sent frame was EXECUTED again. The sender re-sends when its own echo fails, and a failed
//      echo does not mean the frame never arrived (the echo is the transceiver's view of the line,
//      and the bytes can be lost to a full keep/RX ring or to another node starting just after our
//      last bit). A duplicated Save All appends two file-table records per execution and an
//      invalidated record is never reused, so the store filled up and answered FLAG_FAIL.
//      Core/Functions/BusRepeat.h remembers the last guarded request and its status; a repeat is
//      answered from that record instead of being executed.
//
//   2. The backoff was `(RawRand() % 16) + 1` ms: 1-16 ms against a 2.80 ms frame, drawn from a
//      low nibble whose cycle is exactly 16, so two nodes in phase re-drew the same delay on
//      attempts 2 and 3 and collided again in lockstep. The first replacement grouped the delay
//      from `Addr mod 8` and `Addr mod 4` (cards t_763b4cde, t_d7802076), which is a lockstep of
//      period 8: nodes 1 and 9, or 0.2 and 0.10, drew the identical delay for ever.
//
// This test drives the real header (not a model): the guarded-CID predicate, the record/hit/note
// logic, and the real RS485BackoffMs over the real RawRand sequence. The dispatcher wiring (that a
// hit SKIPS execution, that SendResponse notes the outcome) is a device-layer property that cannot
// be linked on the host, so it is pinned by source guards in test/native/run.sh.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "Blocks/DeviceInfo.h" // DeviceStatusStruct, which Packet.h forward-uses
#include "Core/Functions/Bus.h"       // RS485BackoffUs, the silence constants
#include "Core/Functions/BusRepeat.h" // BusRepeatGuarded / Hit / Note

#include "check.h"

static const uint16_t CoreAddr = 0x0001; // the requester (the core)
static const uint16_t NodeAddr = 0x0003; // this node

// A request frame, as the node would receive it.
static PacketFrame Request(uint16_t Src, uint16_t Tgt, ServiceType Svc, uint8_t Cid, uint16_t Trid,
                           uint8_t Flags = FLAG_REQACK)
{
    PacketFrame F;
    std::memset(&F, 0, sizeof(F));
    F.Flags = Flags;
    F.Priority = DEFAULT_PRIORITY;
    F.PayloadLen = 0;
    F.IdSrc = Src;
    F.IdTgt = Tgt;
    F.Cmd = MakeService(Svc, Cid);
    F.Trid = Trid;
    return F;
}

static const uint16_t CmdSaveAll = MakeService(ServiceType::Register, 5);
static const uint16_t CmdRecallAll = MakeService(ServiceType::Register, 4);
static const uint16_t CmdFormat = MakeService(ServiceType::Storage, 0);
static const uint16_t CmdCreate = MakeService(ServiceType::Storage, 1);
static const uint16_t CmdDelete = MakeService(ServiceType::Storage, 2);
static const uint16_t CmdResize = MakeService(ServiceType::Storage, 3);
static const uint16_t CmdRename = MakeService(ServiceType::Storage, 4);
static const uint16_t CmdRead = MakeService(ServiceType::Storage, 5);
static const uint16_t CmdWrite = MakeService(ServiceType::Storage, 6);

// ------------------------------------------------------------------------------------------------
// 1. Which CIDs must be guarded: exactly the ones whose execution appends to / rewrites the flash
//    file table (StorageBlockFS.h): Save All (staging create + rename = 2 records) and the five
//    file-table commands. Everything else is idempotent or guarded elsewhere, and guarding it would
//    break its reply (a read's stream, a write fragment's 2-byte sequence ack).
// ------------------------------------------------------------------------------------------------
static void TestGuardedCids()
{
    CHECK(BusRepeatGuarded(CmdSaveAll));
    CHECK(BusRepeatGuarded(CmdFormat));
    CHECK(BusRepeatGuarded(CmdCreate));
    CHECK(BusRepeatGuarded(CmdDelete));
    CHECK(BusRepeatGuarded(CmdResize));
    CHECK(BusRepeatGuarded(CmdRename));

    // Idempotent (RAM only) or already guarded by the handler itself:
    CHECK(!BusRepeatGuarded(CmdRecallAll));
    CHECK(!BusRepeatGuarded(CmdRead));
    CHECK(!BusRepeatGuarded(CmdWrite));
    // No flash append at all:
    for (uint8_t Cid = 0; Cid < 8; Cid++)
        CHECK(!BusRepeatGuarded(MakeService(ServiceType::Device, Cid)));
    CHECK(!BusRepeatGuarded(MakeService(ServiceType::Subscriptions, 1)));
    CHECK(!BusRepeatGuarded(MakeService(ServiceType::LogHandler, 0)));
    CHECK(!BusRepeatGuarded(MakeService(ServiceType::Script, 1)));
    // Register CIDs other than Save All.
    for (uint8_t Cid = 0; Cid < 16; Cid++)
        CHECK_EQ(BusRepeatGuarded(MakeService(ServiceType::Register, Cid)), Cid == 5);
}

// ------------------------------------------------------------------------------------------------
// 2. The record: a repeat inside the window is a hit and replays the recorded status; anything that
//    is not the same transaction is a miss and executes normally.
// ------------------------------------------------------------------------------------------------
static void TestRepeatRecord()
{
    // Nothing executed yet.
    PacketFrame Save = Request(CoreAddr, NodeAddr, ServiceType::Register, 5, 0xF123);
    CHECK(!BusRepeatHit(Save, 1000));

    // The node executed it and answered SUCCESS at t=1000, then the sender's echo failed.
    BusRepeatNote(Save, FLAG_SUCCESS, 0, 1000);
    CHECK(BusRepeatHit(Save, 1000));
    CHECK(BusRepeatHit(Save, 1200)); // the retry arrives ~4-20 ms later in practice
    CHECK_EQ(BusRepeatStatus(), FLAG_SUCCESS);

    // Outside the window it is a NEW transaction, not a retransmission (a reused TRID must not be
    // swallowed): 400 ms is exactly the boundary, one more millisecond is not.
    CHECK(BusRepeatHit(Save, 1000 + BUS_REPEAT_WINDOW_MS));
    CHECK(!BusRepeatHit(Save, 1000 + BUS_REPEAT_WINDOW_MS + 1));

    // Different TRID: a new transaction.
    PacketFrame Other = Request(CoreAddr, NodeAddr, ServiceType::Register, 5, 0xF124);
    CHECK(!BusRepeatHit(Other, 1010));

    // Different source: another device's transaction.
    PacketFrame FromElsewhere = Request(0x0004, NodeAddr, ServiceType::Register, 5, 0xF123);
    CHECK(!BusRepeatHit(FromElsewhere, 1010));

    // Same TRID, different command: a reused TRID must not borrow another command's status.
    PacketFrame OtherCmd = Request(CoreAddr, NodeAddr, ServiceType::Storage, 1, 0xF123);
    CHECK(!BusRepeatHit(OtherCmd, 1010));

    // A FAILED execution is replayed as a failure, not re-executed (it would fail again and append
    // nothing, but the record must not turn a refusal into a success).
    PacketFrame Create = Request(CoreAddr, NodeAddr, ServiceType::Storage, 1, 0xF200);
    BusRepeatNote(Create, FLAG_FAIL, 0, 2000);
    CHECK(BusRepeatHit(Create, 2000));
    CHECK_EQ(BusRepeatStatus(), FLAG_FAIL);

    // The 16-bit clock wraps: a repeat across the wrap is still inside its window (a 32-bit clock
    // read truncated to 16 bits must stay wrap-safe, or the guard would go blind every 65 s).
    PacketFrame Late = Request(CoreAddr, NodeAddr, ServiceType::Register, 5, 0xF300);
    BusRepeatNote(Late, FLAG_SUCCESS, 0, 0xFF00);
    CHECK(BusRepeatHit(Late, (uint16_t)(0xFF00 + 200)));  // 0xFFC8, no wrap yet
    CHECK(BusRepeatHit(Late, (uint16_t)(0xFF00 + 400)));  // 0x0090 - across the wrap, delta 400
    CHECK(!BusRepeatHit(Late, (uint16_t)(0xFF00 + 401))); // 0x0091 - one ms past the window
}

// ------------------------------------------------------------------------------------------------
// 3. What must NOT be recorded: a reply with a payload, an unguarded CID, or a frame that is itself
//    a reply. Each of those would let the replay answer with a frame the original never sent.
// ------------------------------------------------------------------------------------------------
static void TestRecordRefuses()
{
    // Reset the record to "nothing executed" - Status 0 is the empty marker.
    BusRepeatLast = BusRepeatEntry{0, 0, 0, 0, 0};

    // A guarded request whose reply carried a payload: cannot be rebuilt from one status byte.
    PacketFrame Save = Request(CoreAddr, NodeAddr, ServiceType::Register, 5, 0xF400);
    BusRepeatNote(Save, FLAG_SUCCESS, 2, 5000);
    CHECK(!BusRepeatHit(Save, 5000));

    // An unguarded command (a storage read) is never recorded.
    PacketFrame Read = Request(CoreAddr, NodeAddr, ServiceType::Storage, 5, 0xF401);
    BusRepeatNote(Read, FLAG_SUCCESS, 0, 5100);
    CHECK(!BusRepeatHit(Read, 5100));

    // A response frame is not a request.
    PacketFrame Reply = Request(CoreAddr, NodeAddr, ServiceType::Register, 5, 0xF402, FLAG_TYPE);
    BusRepeatNote(Reply, FLAG_SUCCESS, 0, 5200);
    CHECK(!BusRepeatHit(Reply, 5200));

    // A zero status is "nothing recorded", so it can never be replayed as a status.
    PacketFrame Z = Request(CoreAddr, NodeAddr, ServiceType::Register, 5, 0xF403);
    BusRepeatNote(Z, 0, 0, 5300);
    CHECK(!BusRepeatHit(Z, 5300));
}

// ------------------------------------------------------------------------------------------------
// 4. The backoff: a millisecond grid wider than one frame, and a pair of nodes that collided
//    separates on the FIRST retry (card t_d7802076).
// ------------------------------------------------------------------------------------------------
static void TestBackoffRange()
{
    // One frame at 460800 8N1: (1 sync + 12 header + 116 payload) * 10 bits / 460800 = 2800 us,
    // i.e. 3 ms as the backoff grid counts it (both targets sleep in whole milliseconds).
    CHECK_EQ(RS485_FRAME_US, 2800);
    CHECK_EQ(RS485_FRAME_MS, 3);
    // A backoff step must be wider than the silence window the retry waits out afterwards, or the
    // separation is absorbed and the pair re-collides. The window is 8 + (priority/8) + 0..3 byte
    // times = at most 12 * 22 us with the lowest priority of 15.
    CHECK(1000 > (RS485_SILENCE_BYTES + 15 / 8 + 3) * RS485_BYTE_TIME_US);

    uint32_t Min = 0xFFFFFFFF, Max = 0;
    for (uint16_t Addr = 1; Addr <= 8; Addr++)
        for (uint8_t Attempt = 0; Attempt < RS485_RETRIES; Attempt++)
            for (int i = 0; i < 64; i++)
            {
                const uint32_t Ms = RS485BackoffMs(Addr, Attempt, RawRand());
                if (Ms < Min) Min = Ms;
                if (Ms > Max) Max = Ms;
            }
    // 2 ms + (slot 0..31) * 1 = 2 .. 33 ms: 0.7x to 11.8x one frame. The window is deliberately
    // the SAME as the first replacement's (2..33), so the transmit stint's worst case, its comment
    // in Devices/Tamu_v2.0A/RSBus.h and the budget arithmetic do not move.
    std::printf("  backoff window: %u .. %u ms (one frame = %u ms)\n", Min, Max, (unsigned)RS485_FRAME_MS);
    CHECK_EQ(Min, 2);
    CHECK_EQ(Max, 33);
    CHECK(Min * 1000 > (RS485_SILENCE_BYTES + 15 / 8 + 3) * RS485_BYTE_TIME_US);
    CHECK(Max >= 3 * RS485_FRAME_MS);
}

static void TestBackoffSeparates()
{
    // THE property the card asks for: two colliding nodes separate within ONE attempt.
    //
    // Two nodes that collided draw at the same call index - they are in phase, so they pass the
    // SAME PRNG word and the same attempt number, and their delays are equal only when their
    // ADDRESS PHASE is equal. Everything therefore reduces to the fold's kernel, and the test
    // sweeps it exhaustively instead of trusting a sample of addresses: for every XOR difference
    // an address pair can have, over every attempt and 32 draws, count the pairs that land on the
    // same step.
    unsigned SameInBlock = 0;  // differences 1..32 that land on the same step: must be 0
    uint32_t Degenerate[4] = {0, 0, 0, 0};
    unsigned DegenerateCount = 0;
    unsigned WorstInBlock = 0xFFFFFFFF;
    uint32_t FirstDegenerate = 0;
    for (uint32_t D = 1; D <= 1024; D++) // one net's device-ID space (10 bits)
    {
        // The phase is linear over GF(2), so phase(A) ^ phase(B) == phase(A ^ B): a base of 0 and
        // the difference D stand for every pair (A, A ^ D).
        bool AnyEqual = false;
        for (uint8_t Attempt = 0; Attempt < RS485_RETRIES; Attempt++)
            for (int i = 0; i < 32; i++)
            {
                const uint32_t Rand = 0x9E3779B9u * (uint32_t)(i + 1);
                const uint32_t Da = RS485BackoffMs(0, Attempt, Rand);
                const uint32_t Db = RS485BackoffMs((uint16_t)D, Attempt, Rand);
                if (Da == Db)
                {
                    AnyEqual = true;
                    continue;
                }
                if (D <= 32) // the block the card must guarantee: how close can a pair land?
                {
                    const uint32_t Delta = (Da > Db ? Da - Db : Db - Da);
                    if (Delta < WorstInBlock) WorstInBlock = Delta;
                }
            }
        if (AnyEqual)
        {
            DegenerateCount++;
            if (D <= 32) SameInBlock++;
            if (FirstDegenerate == 0) FirstDegenerate = D;
            if (DegenerateCount <= 4) Degenerate[DegenerateCount - 1] = D;
        }
    }
    std::printf("  backoff, in-phase draws: differences 1..32 all separated (%u on the same step), "
                "smallest degenerate XOR difference %u, %u degenerate of 1024 inside one net, "
                "worst |delta| %u ms\n",
                SameInBlock, FirstDegenerate, DegenerateCount, WorstInBlock);
    CHECK_EQ(SameInBlock, 0);
    CHECK_EQ(FirstDegenerate, 33);
    // Inside one net the degenerate differences are exactly the multiples of 33: the smallest pair
    // is node 1 and node 32 (1 ^ 32 == 33), and a bus whose node numbers sit in one 32-address
    // block - every real one, allocation starts at 2 - cannot draw the same delay twice.
    CHECK_EQ(DegenerateCount, 31);
    for (unsigned i = 0; i < 4; i++)
        CHECK_EQ(Degenerate[i], 33u * (i + 1));
    CHECK(WorstInBlock >= 1); // the pairs that DO separate are never closer than one grid step

    // The regression this card is about: under the first replacement the group read `Addr mod 8`
    // and the sub term `Addr & 3`, so nodes 1 and 9 (and every pair 8 apart) drew the identical
    // delay for every draw and every attempt - a 8-address lockstep. Recomputed from that formula
    // here so the claim is measured, then checked against the real helper.
    auto OldLockstep = [](uint16_t A, uint16_t B) {
        for (uint8_t Attempt = 0; Attempt < RS485_RETRIES; Attempt++)
            for (int i = 0; i < 64; i++)
            {
                const uint32_t Rand = 0x1234567u * (uint32_t)(i + 1);
                const uint32_t Da = 2 + (((A * 5 + Attempt) & 7) * 4) + (((Rand >> 16) ^ A) & 3);
                const uint32_t Db = 2 + (((B * 5 + Attempt) & 7) * 4) + (((Rand >> 16) ^ B) & 3);
                if (Da != Db) return false;
            }
        return true;
    };
    auto NewSeparates = [](uint16_t A, uint16_t B) {
        for (uint8_t Attempt = 0; Attempt < RS485_RETRIES; Attempt++)
            for (int i = 0; i < 64; i++)
            {
                const uint32_t Rand = 0x1234567u * (uint32_t)(i + 1);
                if (RS485BackoffMs(A, Attempt, Rand) == RS485BackoffMs(B, Attempt, Rand))
                    return false;
            }
        return true;
    };
    CHECK(OldLockstep(1, 9));      // the defect, in the formula that shipped in 52c07aa
    CHECK(NewSeparates(1, 9));     // ... and gone in the address-phase fold
    // The core (1) against every plausible node number. Node 32 is the ONE skipped case and it is
    // the documented residual: 1 ^ 32 == 33 is the smallest degenerate difference, so that pair is
    // still separated only to the extent the two PRNG states differ.
    for (uint16_t B = 2; B <= 33; B++)
    {
        if (B == 32)
            continue;
        CHECK(NewSeparates(1, B));
    }
    CHECK(OldLockstep(1, 33)); // the old mod-8 group locked a pair 32 apart as well

    // Nodes draw from their OWN PRNG state, so a pair can land on the same step by chance: 1 draw
    // in 32 for two independent draws over the 32 slots. Measured here, asserted under 5 %.
    unsigned Nearby = 0, Pairs = 0;
    for (uint16_t A = 1; A <= 8; A++)
        for (uint16_t B = (uint16_t)(A + 1); B <= 8; B++)
            for (uint8_t Attempt = 0; Attempt < RS485_RETRIES; Attempt++)
                for (int i = 0; i < 256; i++)
                {
                    const uint32_t RandA = 0x2468ACE1u * (uint32_t)(i + 1);
                    const uint32_t RandB = 0x13572468u * (uint32_t)(i + 7);
                    const uint32_t Da = RS485BackoffMs(A, Attempt, RandA);
                    const uint32_t Db = RS485BackoffMs(B, Attempt, RandB);
                    Pairs++;
                    if (Da == Db) Nearby++;
                }
    std::printf("  backoff, independent draws: %u of %u pair-draws on the same step (%.2f %%)\n",
                Nearby, Pairs, 100.0 * (double)Nearby / (double)Pairs);
    CHECK(Nearby * 20 <= Pairs); // measured ~3 %, asserted under 5 %
    CHECK(Nearby * 50 >= Pairs); // ... and no better than the 1-in-32 the 32-slot grid allows (>=2 %)

    // The pair that mattered on this rig - nodes 0.3 and 0.4, one address apart - never shares a
    // step, for any draw and attempt.
    for (uint8_t Attempt = 0; Attempt < RS485_RETRIES; Attempt++)
        for (uint32_t Hi = 0; Hi < 32; Hi++)
        {
            const uint32_t RandA = (Hi << 16) | 0x00001234u;
            const uint32_t RandB = (Hi << 16) | 0x00005678u;
            const uint32_t Da = RS485BackoffMs(3, Attempt, RandA);
            const uint32_t Db = RS485BackoffMs(4, Attempt, RandB);
            CHECK(Da != Db);
        }
}

// ------------------------------------------------------------------------------------------------
// 5. The delay is not a fixed cycle of the PRNG's low nibble. The old expression repeated with
//    period 16 because it read (Rand % 16); two nodes in phase re-drew the same value on attempts 2
//    and 3. The new one reads the top half and folds in the address and attempt.
// ------------------------------------------------------------------------------------------------
static void TestBackoffNotA16Cycle()
{
    uint32_t Seq[64];
    for (int i = 0; i < 64; i++)
        Seq[i] = RS485BackoffMs(NodeAddr, 1, RawRand());

    // Distinct values seen: the sub term has four values, and all four appear over 64 draws. The
    // old low nibble produced all 16 values too - but always in the same 16-step order.
    unsigned Distinct = 0;
    for (int i = 0; i < 64; i++)
    {
        bool Seen = false;
        for (int j = 0; j < i; j++)
            if (Seq[j] == Seq[i]) { Seen = true; break; }
        if (!Seen) Distinct++;
    }
    std::printf("  backoff sequence: %u distinct delays over 64 draws\n", Distinct);
    CHECK(Distinct >= 3);

    // No period 4, 8 or 16 reproduces 64 consecutive draws.
    for (int P : {4, 8, 16})
    {
        bool Periodic = true;
        for (int i = 0; i + P < 64; i++)
            if (Seq[i] != Seq[i + P]) { Periodic = false; break; }
        CHECK(!Periodic);
    }

    // The old formula, for contrast: its low nibble is the LCG s' = 13s + 9 (mod 16), a fixed cycle
    // of 16 - so two nodes drawing at the same call index (in phase) re-drew the SAME delay on
    // every attempt. Recomputed here from the header's own LCG constants so the claim in the
    // comment is a measurement rather than an assertion about history.
    uint32_t State = 1;
    for (int i = 0; i < 16; i++)
        State = State * 1103515245u + 12345u;
    const uint32_t First = State % 16;
    for (int i = 0; i < 16; i++) State = State * 1103515245u + 12345u;
    CHECK_EQ(State % 16, First); // period 16 in the nibble the old expression read
}

int main()
{
    TestGuardedCids();
    TestRepeatRecord();
    TestRecordRefuses();
    TestBackoffRange();
    TestBackoffSeparates();
    TestBackoffNotA16Cycle();

    std::printf("%s: %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
