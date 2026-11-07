// Host-side test for the BLE RX chunk assembler (Blocks/BleRxAssembler.h).
//
// The two ways one bad write used to end a session silently, and what replaces them:
//
//  1. a length that cannot be carried (a desync'd prefix) - refused, counted, and the byte that
//     showed it starts a fresh prefix;
//  2. a chunk that stops arriving (a truncated write) - abandoned on the clock, counted, and the
//     next chunk is read from its OWN prefix.
//
// The last check in each repair case is the one that matters in the field: a complete, CRC-valid
// PacketFrame fed AFTER the damage must parse. The parser is the same six lines the device's
// WireStreamParser uses (Devices/Tamu_v2.0A/AppUSB.h): accumulate until PacketWireSize() bytes and
// accept when the CRC8 over Flags..payload matches.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdio>
#include <cstring>

#include "Blocks/BleRxAssembler.h"
#include "Blocks/DeviceInfo.h" // DeviceStatusStruct, which Packet.h forward-uses
#include "Core/Functions/Packet.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                            \
    } while (0)

// The device's packet-stream parser, in miniature (same rules as WireStreamParser::Feed).
struct StreamParser
{
    PacketFrame Frame;
    uint16_t Got = 0;
    bool Full = false;
    int Frames = 0;
    int Rejects = 0;

    void Reset()
    {
        Got = 0;
        Full = false;
    }

    void Feed(uint8_t Byte)
    {
        if (Full)
            Reset();
        ((uint8_t *)&Frame)[Got++] = Byte;
        if (Got < 4)
            return;
        const uint16_t Need = PacketWireSize(&Frame);
        if (Got < Need)
            return;
        Full = true;
        if (Crc8(&Frame.Flags, (uint16_t)(11 + PayloadBytes(Frame))) == Frame.Crc8)
            Frames++;
        else
            Rejects++;
    }
};

// One chunk as it goes on the air: uint16 LE length + that many stream bytes.
struct Chunk
{
    uint8_t Buf[2 + MAX_PAYLOAD_SIZE];
    uint16_t Len = 0;

    void Begin(uint16_t Declared)
    {
        Buf[0] = (uint8_t)(Declared & 0xFF);
        Buf[1] = (uint8_t)(Declared >> 8);
        Len = 2;
    }
    void Push(uint8_t Byte) { Buf[Len++] = Byte; }
};

// A real app frame: a System/Enumerate request, finalised so its CRC8 is correct.
static uint16_t BuildFrame(PacketFrame *Frame, uint8_t PayloadLen)
{
    memset(Frame, 0, sizeof(*Frame));
    Frame->Flags = FLAG_REQACK;
    Frame->Priority = DEFAULT_PRIORITY;
    Frame->IdSrc = 0x1234;
    Frame->IdTgt = 0x0001;
    Frame->Cmd = MakeService(ServiceType::Register, 1);
    Frame->Trid = 7;
    for (uint8_t i = 0; i < PayloadLen; i++)
        Frame->Payload[i] = (uint8_t)(0x40 + i);
    PacketFinalize(Frame, PayloadLen);
    return PacketWireSize(Frame);
}

// Feeds `Count` raw bytes through the assembler into the parser and returns how many resyncs the
// assembler reported.
static uint32_t FeedBytes(BleRxAssembler &A, StreamParser &P, const uint8_t *Bytes, uint16_t Count,
                          uint32_t NowMs)
{
    uint32_t Resyncs = 0;
    for (uint16_t i = 0; i < Count; i++)
    {
        uint8_t Out = 0;
        const BleFeedResult R = A.Feed(Bytes[i], NowMs, &Out);
        if (R.Resynced)
        {
            Resyncs++;
            P.Reset(); // what AppBLE.h onWrite does
        }
        if (R.Stream)
            P.Feed(Out);
    }
    return Resyncs;
}

// One complete chunk in one write (the normal case), plus the two shapes BlueZ actually produces:
// fragmented (one chunk over several writes) and coalesced (several chunks in one write).
static void TestNormalAndFramingShapes()
{
    PacketFrame Frame;
    const uint16_t FrameLen = BuildFrame(&Frame, 24);
    const uint8_t *Wire = (const uint8_t *)&Frame;

    // NORMAL: one chunk carrying the whole frame.
    {
        BleRxAssembler A;
        StreamParser P;
        Chunk C;
        C.Begin(FrameLen);
        for (uint16_t i = 0; i < FrameLen; i++)
            C.Push(Wire[i]);
        CHECK(FeedBytes(A, P, C.Buf, C.Len, 100) == 0);
        CHECK(P.Frames == 1);
        CHECK(A.Resyncs == 0);
    }

    // FRAGMENTED: the same chunk delivered four bytes at a time.
    {
        BleRxAssembler A;
        StreamParser P;
        Chunk C;
        C.Begin(FrameLen);
        for (uint16_t i = 0; i < FrameLen; i++)
            C.Push(Wire[i]);
        uint32_t Resyncs = 0;
        for (uint16_t Off = 0; Off < C.Len; Off += 4)
        {
            const uint16_t N = (uint16_t)((C.Len - Off > 4) ? 4 : (C.Len - Off));
            Resyncs += FeedBytes(A, P, C.Buf + Off, N, 100);
        }
        CHECK(Resyncs == 0);
        CHECK(P.Frames == 1);
    }

    // COALESCED: two chunks in one write.
    {
        BleRxAssembler A;
        StreamParser P;
        uint8_t Big[2 * (2 + MAX_PAYLOAD_SIZE)];
        uint16_t Len = 0;
        for (int Copy = 0; Copy < 2; Copy++)
        {
            Chunk C;
            C.Begin(FrameLen);
            for (uint16_t i = 0; i < FrameLen; i++)
                C.Push(Wire[i]);
            memcpy(Big + Len, C.Buf, C.Len);
            Len = (uint16_t)(Len + C.Len);
        }
        CHECK(FeedBytes(A, P, Big, Len, 100) == 0);
        CHECK(P.Frames == 2);
    }

    // A zero-length chunk is dropped and the next chunk still frames (no resync counted: there is
    // nothing to abandon, the prefix was simply empty).
    {
        BleRxAssembler A;
        StreamParser P;
        Chunk C;
        C.Begin(FrameLen);
        for (uint16_t i = 0; i < FrameLen; i++)
            C.Push(Wire[i]);
        uint8_t WithEmpty[2 + 2 + MAX_PAYLOAD_SIZE];
        WithEmpty[0] = 0;
        WithEmpty[1] = 0;
        memcpy(WithEmpty + 2, C.Buf, C.Len);
        CHECK(FeedBytes(A, P, WithEmpty, (uint16_t)(2 + C.Len), 100) == 0);
        CHECK(P.Frames == 1);
        CHECK(A.Resyncs == 0);
    }
}

// PROOF (ii), the truncated write: a chunk declares N and delivers fewer. Without the clock, the
// next chunk's 2-byte prefix would be eaten as payload and every later frame would fail CRC - the
// session's RX was dead with nothing in the log to say so.
static void TestTruncatedChunkResyncs()
{
    BleRxAssembler A;
    StreamParser P;

    PacketFrame Frame;
    const uint16_t FrameLen = BuildFrame(&Frame, 40);
    const uint8_t *Wire = (const uint8_t *)&Frame;

    // The damage: a chunk that declares 32 stream bytes and carries 5 of them (the sender died
    // mid-write, or the stack truncated the write).
    Chunk Short;
    Short.Begin(32);
    for (int i = 0; i < 5; i++)
        Short.Push(0xA0);
    CHECK(FeedBytes(A, P, Short.Buf, Short.Len, 1000) == 0); // nothing to report yet: still waiting
    CHECK(A.Truncations == 0);
    CHECK(A.HaveLen);

    // The next write arrives after the timeout. The clock abandons the partial chunk, the resync is
    // counted, and this write's first byte is read as a PREFIX (not as payload).
    Chunk Next;
    Next.Begin(FrameLen);
    for (uint16_t i = 0; i < FrameLen; i++)
        Next.Push(Wire[i]);
    const uint32_t Resyncs = FeedBytes(A, P, Next.Buf, Next.Len, 1000 + BLE_RX_CHUNK_TIMEOUT_MS + 1);

    CHECK(Resyncs == 1);
    CHECK(A.Resyncs == 1);
    CHECK(A.Truncations == 1);
    CHECK(A.Overruns == 0);
    // The frame that followed the truncation PARSES - this is the whole point of the counter.
    CHECK(P.Frames == 1);
    CHECK(P.Rejects == 0);
}

// A truncated chunk that is continued INSIDE the timeout is not a truncation: a slow, fragmented
// writer must not be punished for a gap the clock has not reached yet.
static void TestSlowChunkInsideTimeoutSurvives()
{
    BleRxAssembler A;
    StreamParser P;

    PacketFrame Frame;
    const uint16_t FrameLen = BuildFrame(&Frame, 12);
    const uint8_t *Wire = (const uint8_t *)&Frame;

    Chunk C;
    C.Begin(FrameLen);
    for (uint16_t i = 0; i < FrameLen; i++)
        C.Push(Wire[i]);

    // The prefix and half the payload, a 900 ms gap, then the rest - still inside
    // BLE_RX_CHUNK_TIMEOUT_MS, so it is the same chunk finishing.
    const uint16_t Split = (uint16_t)(2 + FrameLen / 2);
    CHECK(FeedBytes(A, P, C.Buf, Split, 1000) == 0);
    CHECK(FeedBytes(A, P, C.Buf + Split, (uint16_t)(C.Len - Split), 1900) == 0);
    CHECK(P.Frames == 1);
    CHECK(A.Resyncs == 0);
}

// A declared length the link cannot carry is not a length: it is a desync. Both bounds are checked -
// the absolute ATT ceiling and the negotiated MTU's own limit.
static void TestOversizeLengthIsRefused()
{
    // The absolute ceiling: 507 stream bytes is the most a 512-byte ATT MTU can carry.
    {
        BleRxAssembler A;
        StreamParser P;
        Chunk C;
        C.Begin((uint16_t)(BLE_STREAM_CHUNK_ABS_MAX + 1));
        CHECK(FeedBytes(A, P, C.Buf, C.Len, 100) == 1);
        CHECK(A.Overruns == 1);
        CHECK(A.Truncations == 0);
        CHECK(!A.HaveLen); // no phantom payload is expected
    }

    // The link's own bound: with a negotiated MTU of 247 a chunk can carry at most 242.
    {
        BleRxAssembler A;
        A.MaxNeed = BleMaxChunkForMtu(247);
        CHECK(A.MaxNeed == 242);
        Chunk C;
        C.Begin(243);
        StreamParser P;
        CHECK(FeedBytes(A, P, C.Buf, C.Len, 100) == 1);
        CHECK(A.Overruns == 1);
    }

    // ... and the value at the bound is accepted.
    {
        BleRxAssembler A;
        A.MaxNeed = BleMaxChunkForMtu(247);
        StreamParser P;
        Chunk C;
        C.Begin(242);
        for (int i = 0; i < 242; i++)
            C.Push((uint8_t)i);
        CHECK(FeedBytes(A, P, C.Buf, C.Len, 100) == 0);
        CHECK(A.Overruns == 0);
        CHECK(A.HaveLen == false); // the chunk completed
    }

    // An unknown MTU never tightens the bound (a stale low reading must not refuse real writes).
    CHECK(BleMaxChunkForMtu(23) == 18);
    CHECK(BleMaxChunkForMtu(0) == BLE_STREAM_CHUNK_ABS_MAX);
    CHECK(BleMaxChunkForMtu(5) == BLE_STREAM_CHUNK_ABS_MAX);
    CHECK(BleMaxChunkForMtu(512) == BLE_STREAM_CHUNK_ABS_MAX);
}

// The oversize case end to end: the damage, then a good frame in the very next write parses.
static void TestOversizeThenFrameParses()
{
    BleRxAssembler A;
    StreamParser P;

    PacketFrame Frame;
    const uint16_t FrameLen = BuildFrame(&Frame, 8);
    const uint8_t *Wire = (const uint8_t *)&Frame;

    Chunk Bad;
    Bad.Begin(0xFFFF); // what a corrupt prefix looked like
    const uint32_t Resyncs = FeedBytes(A, P, Bad.Buf, Bad.Len, 100);
    CHECK(Resyncs == 1);

    Chunk Good;
    Good.Begin(FrameLen);
    for (uint16_t i = 0; i < FrameLen; i++)
        Good.Push(Wire[i]);
    CHECK(FeedBytes(A, P, Good.Buf, Good.Len, 101) == 0);
    CHECK(P.Frames == 1);
    CHECK(A.Resyncs == 1);
}

// A session boundary (connect/disconnect) clears the state but is not a resync: the counters only
// move on damage, so a quiet link never looks damaged.
static void TestSessionResetKeepsCounters()
{
    BleRxAssembler A;
    Chunk C;
    C.Begin(10);
    C.Push(1);
    uint8_t Out = 0;
    A.Feed(C.Buf[0], 100, &Out);
    A.Feed(C.Buf[1], 100, &Out);
    A.Feed(C.Buf[2], 100, &Out);
    CHECK(A.HaveLen);
    CHECK(A.InChunk);

    A.Reset();
    CHECK(!A.HaveLen);
    CHECK(!A.InChunk);
    CHECK(A.Resyncs == 0);
    CHECK(A.Overruns == 0);
    CHECK(A.Truncations == 0);
}

int main()
{
    TestNormalAndFramingShapes();
    TestTruncatedChunkResyncs();
    TestSlowChunkInsideTimeoutSurvives();
    TestOversizeLengthIsRefused();
    TestOversizeThenFrameParses();
    TestSessionResetKeepsCounters();

    printf("ble rx assembler: %d checks passed", checks - failures);
    if (failures != 0)
        printf(", %d FAILED", failures);
    printf("\n");
    return failures == 0 ? 0 : 1;
}
