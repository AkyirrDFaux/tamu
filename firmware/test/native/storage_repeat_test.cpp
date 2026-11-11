// Host-side test for the retransmission dedupe path on a RECEIVING service (card t_8dead2f1).
//
// The bus re-sends a frame when its own ECHO fails (Devices/*/RSBus.h SendAndVerifyPacket), and a
// failed echo does not mean the frame never arrived: the echo is the transceiver's own view of the
// line, so it can be eaten by a full RX ring or by another node starting just after the last bit.
// The receiver then EXECUTED the request a second time - and every file-table mutation APPENDS a
// record (StorageBlockFS.h WriteFileRecord), with invalidated records never reused (DeleteMatching
// zeroes the offset, so a hole is dead space). A duplicated Save All therefore spends four of a
// node's eight table slots instead of two, and a few of them walk the store into the full-table
// state that answers FLAG_FAIL (the 0x40/0x4E refusals measured on nodes 0.3/0.4).
//
// Core/Functions/BusRepeat.h answers a repeat from a one-record ledger instead of executing it.
// bus_repeat_test.cpp drives that ledger in isolation; THIS test drives the dedupe PATH: the real
// `StorageService::Handle` over the real `StorageBlockFS` on a fake 512 B NOR flash in the DAS
// geometry, so "executed once" is measured the way the node's store measures it - the file table's
// append position (`Storage.GetEndOfFiletable`, the first slot with offset 0xFFFFFFFF), which is
// what a duplicated save spends and what the rig probe read off a live node.
//
// What is pinned, in order:
//   1. a Create executes once: +1 record, one status reply;
//   2. the SAME frame 5 ms later appends NOTHING and is answered with the original reply, byte for
//      byte (this is the whole point: the retransmission is invisible to the store);
//   3. the same bytes under a NEW TRID still execute (+1) - so case 2 is the ledger, not a drop;
//   4. a Resize (which SUCCEEDS every time) costs one record per execution: +1, then +0 for its
//      retransmission, then +1 again for the same bytes under a new TRID. This is the append cost
//      measured on the store, and the assertion that fails without the ledger;
//   5. a Rename's retransmission is answered SUCCESS from the record, where a second EXECUTION
//      would have answered FLAG_FAIL (the old name is gone) - the case that turns a lost echo into
//      an app-visible failure without the guard;
//   6. the window expires: the same frame 401 ms later executes again (a reused TRID is not
//      swallowed for ever);
//   7. another source's frame under the same TRID is a different transaction and executes;
//   8. CIDs the ledger deliberately does NOT cover still behave as the audit says: a Read re-reads
//      (payload, not replayable) and a repeated Write fragment is refused by its own sequence check
//      instead of being applied twice;
//   9. the RAM cost is the stated one record (sizeof(BusRepeatEntry) == 10 B, .bss 12 B aligned).
//
// Built in the DAS geometry (run.sh passes the same four flags storage_backup_test uses), because
// the node is the target whose 2 KB RAM the ledger has to fit in.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Types/Enums.h"
#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Packet.h"

// ---- The device-layer symbols the storage service and the reply builder reference. ----
DeviceStatusStruct DeviceStatus;
PacketFrame TxFrame;

// Every reply the service emits, in order (a status reply and a streamed fragment both land here).
static const int MaxReplies = 16;
static PacketFrame Replies[MaxReplies];
static int ReplyCount = 0;

void DispatchPacket(const PacketFrame &Frame)
{
    if (ReplyCount < MaxReplies) Replies[ReplyCount] = Frame;
    ReplyCount++;
}

// The ledger's clock: the guard compares TimeFromBoot() truncated to 16 bits, so the test drives it
// explicitly instead of waiting in real time.
static uint32_t FakeNowMs = 0;
uint32_t TimeFromBoot() { return FakeNowMs; }

void DeviceLog(const char *, const char *, ...) {}

#include "Core/Functions/MemoryBackup.h" // SendResponse (records a guarded outcome), WriteBackupFile
#include "Core/Services/Storage.h"       // StorageService::Handle - the real dedupe path

// ---- The fake 512 B NOR store the DAS runs on (8 x 64 B pages), shared with the backup test. ----
#include "fake_flash.h"

#include "check.h"

static const uint16_t CoreAddr = 0x0001;     // the requester (the core)
static const uint16_t OtherCore = 0x0401;    // a second core, same TRID space
static const uint16_t NodeAddr = 0x0003;     // this node (the receiver)

// An 8-byte record name, space padded - the wire form of a file name.
static void Name8(char Out[8], const char *Text)
{
    memset(Out, ' ', 8);
    for (int i = 0; i < 8 && Text[i]; i++) Out[i] = Text[i];
}

// The file table's APPEND POSITION: the first slot whose offset is 0xFFFFFFFF. Every executed
// file-table mutation moves it; a mutation that never ran cannot. This is the node-side metric the
// rig probe read off a live `.TABLE`, and it counts consumed slots rather than live files (a save
// invalidates the copy it replaces, so the live count stays flat).
static uint32_t AppendSlot()
{
    uint32_t Slot = 0;
    if (!Storage.GetEndOfFiletable(&Slot)) return 0xFFFFFFFF;
    return Slot;
}

static PacketFrame Request(uint8_t Cid, uint16_t Trid, const char *Name, uint32_t Size = 64,
                           uint16_t Src = CoreAddr)
{
    PacketFrame F;
    memset(&F, 0, sizeof(F));
    F.Flags = FLAG_REQACK;
    F.Priority = DEFAULT_PRIORITY;
    F.IdSrc = Src;
    F.IdTgt = NodeAddr;
    F.Cmd = MakeService(ServiceType::Storage, Cid);
    F.Trid = Trid;
    Name8((char *)F.Payload, Name);
    F.PayloadLen = 8;
    if (Cid == 1 || Cid == 3) { // Create / Resize carry the size after the name
        StoreUnaligned<uint32_t>(F.Payload + 8, Size);
        F.PayloadLen = 12;
    }
    // A storage request the app sends is one whole payload (never a fragment) except CID 6.
    return F;
}

static PacketFrame RenameRequest(uint16_t Trid, const char *From, const char *To,
                                 uint16_t Src = CoreAddr)
{
    PacketFrame F = Request(4, Trid, From, 64, Src);
    Name8((char *)F.Payload + 8, To);
    F.PayloadLen = 16;
    return F;
}

// Two replies are "the same answer" when every echoed field matches: the sender matches on TRID and
// the status flag, and the payload must be the empty one the ledger can rebuild.
static bool SameReply(const PacketFrame &A, const PacketFrame &B)
{
    return A.Flags == B.Flags && A.IdSrc == B.IdSrc && A.IdTgt == B.IdTgt && A.Cmd == B.Cmd &&
           A.Trid == B.Trid && A.PayloadLen == B.PayloadLen;
}

static void ResetReplies() { ReplyCount = 0; memset(Replies, 0, sizeof(Replies)); }

static void ShowReply(const char *What, const PacketFrame &R)
{
    std::printf("  %s: flags=0x%02X status=%s trid=0x%04X payload=%u B\n", What, R.Flags,
                (R.Flags & FLAG_SUCCESS) ? "SUCCESS" : (R.Flags & FLAG_FAIL) ? "FAIL" : "-",
                R.Trid, (unsigned)R.PayloadLen);
}

int main()
{
    std::printf("  RAM cost of the ledger: sizeof(BusRepeatEntry) = %u B (one record)\n",
                (unsigned)sizeof(BusRepeatEntry));
    CHECK_EQ(sizeof(BusRepeatEntry), 10);

    Storage.Init();
    CHECK(Storage.FileTableOffset != 0);
    std::printf("  store: %u B flash, %u B blocks, %u table slots, append position %u\n",
                (unsigned)STORAGE_FLASH_SIZE, (unsigned)STORAGE_BLOCK_SIZE,
                (unsigned)Storage.TableCapacity(), (unsigned)AppendSlot());

    // ---- 1/2/3. A Create: executed once, its retransmission answered from the record. ----
    FakeNowMs = 1000;
    const PacketFrame CreateA = Request(1, 0xF101, "RPT_1", 64);
    uint32_t Before = AppendSlot();
    ResetReplies();
    StorageSvc.Handle(CreateA);
    const uint32_t AfterFirst = AppendSlot();
    CHECK_EQ(ReplyCount, 1);
    ShowReply("create, first execution", Replies[0]);
    CHECK(Replies[0].Flags & FLAG_TYPE);
    CHECK(Replies[0].Flags & FLAG_SUCCESS);
    CHECK_EQ(Replies[0].PayloadLen, 0);
    CHECK_EQ(Replies[0].Trid, 0xF101);
    CHECK_EQ(Replies[0].Cmd, CreateA.Cmd);
    CHECK_EQ(AfterFirst, Before + 1); // one record appended: the file's
    CHECK(Storage.FileExists((const char *)CreateA.Payload) != 0xFFFFFFFF);

    const PacketFrame FirstReply = Replies[0];
    FakeNowMs = 1005; // the retry a failed echo produces, 5 ms later
    ResetReplies();
    StorageSvc.Handle(CreateA);
    const uint32_t AfterRepeat = AppendSlot();
    CHECK_EQ(ReplyCount, 1);
    ShowReply("create, retransmission    ", Replies[0]);
    CHECK_EQ(AfterRepeat, AfterFirst);      // NOTHING appended: the execution did not happen twice
    CHECK(SameReply(Replies[0], FirstReply)); // and the requester gets the original answer
    CHECK(Storage.FileExists((const char *)CreateA.Payload) != 0xFFFFFFFF);

    // The control: the same bytes under a NEW TRID are a new transaction and DO execute. Without
    // this, case 2 could be satisfied by a service that simply dropped the second frame.
    ResetReplies();
    StorageSvc.Handle(Request(1, 0xF102, "RPT_2", 64));
    const uint32_t AfterControl = AppendSlot();
    CHECK_EQ(ReplyCount, 1);
    CHECK(Replies[0].Flags & FLAG_SUCCESS);
    CHECK_EQ(AfterControl, AfterRepeat + 1);
    std::printf("  appends: first %u -> repeat %u (same frame) -> new TRID %u\n",
                (unsigned)AfterFirst, (unsigned)AfterRepeat, (unsigned)AfterControl);

    // ---- 4. The append COST of one execution, on an operation that succeeds twice. ----
    // Create and Rename are store-neutral when re-executed (the second execution fails on a
    // duplicate / a missing name, so it appends nothing) - what a duplicate costs there is a
    // spurious FLAG_FAIL to the requester, which is why the reply replay above is the property to
    // assert. A Resize to a smaller size succeeds EVERY time and appends a replacement record each
    // time, so it is the operation whose duplicate really spends table slots:
    //   first execution +1, the retransmission +0, the same bytes under a new TRID +1 again.
    // Save All is the same shape on a larger scale (staging create + rename = 2 records per
    // execution); it cannot be driven here because the Register service needs the whole block
    // model, but the ledger it consults is the one exercised by this test.
    const PacketFrame Resize = Request(3, 0xF150, "RPT_1", 32);
    Before = AppendSlot();
    FakeNowMs = 1500;
    ResetReplies();
    StorageSvc.Handle(Resize);
    const uint32_t AfterResize = AppendSlot();
    CHECK_EQ(ReplyCount, 1);
    ShowReply("resize, first execution   ", Replies[0]);
    CHECK(Replies[0].Flags & FLAG_SUCCESS);
    CHECK_EQ(AfterResize, Before + 1); // the replacement record
    const PacketFrame ResizeReply = Replies[0];

    FakeNowMs = 1504; // the retransmission
    ResetReplies();
    StorageSvc.Handle(Resize);
    const uint32_t AfterResizeRepeat = AppendSlot();
    CHECK_EQ(ReplyCount, 1);
    ShowReply("resize, retransmission    ", Replies[0]);
    CHECK_EQ(AfterResizeRepeat, AfterResize); // +0: the second execution never happened
    CHECK(SameReply(Replies[0], ResizeReply));

    ResetReplies();
    StorageSvc.Handle(Request(3, 0xF151, "RPT_1", 32)); // new TRID: executes, and costs a record
    CHECK_EQ(ReplyCount, 1);
    ShowReply("resize, new TRID          ", Replies[0]);
    CHECK(Replies[0].Flags & FLAG_SUCCESS);
    CHECK_EQ(AppendSlot(), AfterResizeRepeat + 1);
    std::printf("  resize appends: first %u -> repeat %u (same frame) -> new TRID %u\n",
                (unsigned)AfterResize, (unsigned)AfterResizeRepeat, (unsigned)AppendSlot());

    // ---- 5. A Rename: the retransmission answers SUCCESS, where a second EXECUTION would FAIL. ----
    const PacketFrame Rename = RenameRequest(0xF110, "RPT_2", "RPT_3");
    Before = AppendSlot();
    FakeNowMs = 2000;
    ResetReplies();
    StorageSvc.Handle(Rename);
    const uint32_t AfterRename = AppendSlot();
    CHECK_EQ(ReplyCount, 1);
    CHECK(Replies[0].Flags & FLAG_SUCCESS);
    CHECK_EQ(AfterRename, Before + 1); // the replacement record the rename appends
    CHECK(Storage.FileExists("RPT_3        ") != 0xFFFFFFFF);
    const PacketFrame RenameReply = Replies[0];

    FakeNowMs = 2004;
    ResetReplies();
    StorageSvc.Handle(Rename);
    CHECK_EQ(ReplyCount, 1);
    ShowReply("rename, retransmission    ", Replies[0]);
    CHECK_EQ(AppendSlot(), AfterRename); // no second record
    CHECK(SameReply(Replies[0], RenameReply));

    // The next EXECUTION of the same bytes (new TRID) fails, because the old name is gone: that is
    // exactly the answer the requester would have got for its retransmission without the ledger.
    ResetReplies();
    StorageSvc.Handle(RenameRequest(0xF111, "RPT_2", "RPT_3"));
    CHECK_EQ(ReplyCount, 1);
    ShowReply("rename, re-executed       ", Replies[0]);
    CHECK(Replies[0].Flags & FLAG_FAIL);
    CHECK_EQ(AppendSlot(), AfterRename);

    // ---- 6. The window expires: a reused TRID is a new transaction again. ----
    FakeNowMs = 2004 + BUS_REPEAT_WINDOW_MS + 1;
    ResetReplies();
    StorageSvc.Handle(Rename);
    CHECK_EQ(ReplyCount, 1);
    ShowReply("rename, past the window   ", Replies[0]);
    CHECK(Replies[0].Flags & FLAG_FAIL); // executed again (and fails, the old name is gone)

    // ---- 7. Another source's frame under the same TRID is a different transaction. ----
    FakeNowMs = 3000;
    ResetReplies();
    StorageSvc.Handle(Request(1, 0xF120, "RPT_4", 64));
    CHECK_EQ(ReplyCount, 1);
    CHECK(Replies[0].Flags & FLAG_SUCCESS);
    const uint32_t AfterSrcA = AppendSlot();
    ResetReplies();
    StorageSvc.Handle(Request(1, 0xF120, "RPT_5", 64, OtherCore)); // same TRID, other requester
    CHECK_EQ(ReplyCount, 1);
    CHECK(Replies[0].Flags & FLAG_SUCCESS);
    CHECK_EQ(AppendSlot(), AfterSrcA + 1);

    // ---- 8. What the audit says is NOT guarded still behaves that way. ----
    // Read (CID 5): a payload reply cannot be rebuilt from one status byte, and a read appends
    // nothing, so it is not in the ledger - a retransmitted read re-reads the same bytes.
    ResetReplies();
    StorageSvc.Handle(Request(5, 0xF130, "RPT_4"));
    const int FirstReadReplies = ReplyCount;
    CHECK(FirstReadReplies >= 1);
    CHECK(Replies[0].PayloadLen > 0); // the name echo + content travel in every fragment
    ResetReplies();
    StorageSvc.Handle(Request(5, 0xF130, "RPT_4")); // same frame, same TRID
    CHECK_EQ(ReplyCount, FirstReadReplies); // answered again (not replayed from the record)

    // Write (CID 6): a write fragment touches FILE DATA in place - it appends no file-table record -
    // and the stream's own sequence check is what keeps a repeat from being applied twice. The
    // check has one shape at fragment 0 (a fragment 0 always RE-OPENS the stream and rewrites the
    // same bytes at offset 0) and a strict one for every later fragment (Current must be
    // WriteSeq+1). Both are pinned here, because that difference is what the audit reports.
    // Wire layout of a write fragment (Docs 03.06, B12): [frag info 4][name 8][content].
    const auto Fragment = [](uint16_t Index, uint8_t First) {
        PacketFrame F;
        memset(&F, 0, sizeof(F));
        F.Flags = (uint8_t)(FLAG_REQACK | FLAG_FRAG);
        F.Priority = DEFAULT_PRIORITY;
        F.IdSrc = CoreAddr;
        F.IdTgt = NodeAddr;
        F.Cmd = MakeService(ServiceType::Storage, 6);
        F.Trid = 0xF140;
        StoreUnaligned<uint16_t>(F.Payload, Index);
        StoreUnaligned<uint16_t>(F.Payload + 2, 2); // of 2
        Name8((char *)F.Payload + 4, "RPT_5");
        for (int i = 0; i < 4; i++) F.Payload[12 + i] = (uint8_t)(First + i);
        F.PayloadLen = 12 + 4;
        return F;
    };
    const uint32_t AppendsBeforeWrite = AppendSlot();
    FakeNowMs = 4000;
    ResetReplies();
    StorageSvc.Handle(Fragment(0, 0xA0));
    CHECK_EQ(ReplyCount, 1);
    ShowReply("write fragment 0, first   ", Replies[0]);
    CHECK(!(Replies[0].Flags & FLAG_FAIL));
    CHECK_EQ(Replies[0].PayloadLen, 2); // the sequence ack a status-only replay could not carry
    CHECK_EQ((uint16_t)(Replies[0].Payload[0] | (Replies[0].Payload[1] << 8)), 0);

    ResetReplies();
    StorageSvc.Handle(Fragment(1, 0xA4));
    CHECK_EQ(ReplyCount, 1);
    CHECK(!(Replies[0].Flags & FLAG_FAIL));
    CHECK_EQ((uint16_t)(Replies[0].Payload[0] | (Replies[0].Payload[1] << 8)), 1);

    ResetReplies();
    StorageSvc.Handle(Fragment(1, 0xA4)); // the same fragment retransmitted
    CHECK_EQ(ReplyCount, 1);
    ShowReply("write fragment 1, repeat  ", Replies[0]);
    CHECK(Replies[0].Flags & FLAG_FAIL); // refused out of order: not applied a second time

    uint8_t Landed[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(Storage.ReadFromFile("RPT_5   ", 0, 8, (char *)Landed) == 8);
    CHECK(Landed[0] == 0xA0 && Landed[3] == 0xA3 && Landed[4] == 0xA4 && Landed[7] == 0xA7);
    CHECK_EQ(AppendSlot(), AppendsBeforeWrite); // a write stream spends no table record at all

    std::printf("%s: %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
