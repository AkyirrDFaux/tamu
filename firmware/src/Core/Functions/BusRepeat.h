#pragma once

#include "Core/Functions/Packet.h"

// The monotonic clock the window is measured on: Core/Devices/Base.h on the CH32 targets,
// Devices/Tamu_v2.0A/Base.h on the core. Declared here so this header stays platform-free (the
// native test drives the guard with an explicit NowMs and never needs the definition).
uint32_t TimeFromBoot();

// Repeat guard for retransmitted requests (card t_763b4cde).
//
// The bus's transmit primitive re-sends a frame when its own ECHO fails (Devices/*/RSBus.h). A
// failed echo does not mean the frame never arrived: the echo is the transceiver's own view of the
// line, so a frame that reached the peer intact can still be retransmitted because
//   * the sender's keep ring / the DAS's 260-byte RX ring was full and dropped the echo bytes,
//   * another node began talking just after our last bit and landed inside the echo window, or
//     (before t_4c9c5fa8) the silence wait had drained the echo into the bin.
// The node then executed the same request a second time, and a third.
//
// That is expensive exactly where it must not be: every file-table mutation APPENDS a record
// (StorageBlockFS.h WriteFileRecord) and an invalidated record is never reused (DeleteMatching
// zeroes the offset, "holes are never reused"), so a duplicated Save All spends four of the eight
// table slots instead of two - staging create + rename per execution - and walks the store into
// the full-table state that answers FLAG_FAIL: the 0x40/0x4E refusals measured on nodes 0.3/0.4.
//
// So the RECEIVING side remembers the last request it executed whose execution appends to flash,
// and answers a repeat of it from that record instead of executing it again. The record keeps the
// reply's status byte, and the replay is the same frame the original execution sent
// (FLAG_TYPE|START|STOP|status, no payload) - which IS the whole reply of every guarded CID, so
// nothing is lost by not keeping a payload.
//
// Why not option (b), "retry only when the frame is shown not to have been delivered"? Because the
// only delivery signal the sender has is that same echo, and the echo cannot separate "the peer
// never got a valid frame" from "the peer got it and the echo was spoiled": both present as a
// mismatched or absent echo. There is no wire-level acknowledgement to fall back on (a request
// without FLAG_REQACK is answered by nobody at all), and adding one would change the packet
// specification. So the sender cannot know, and the receiver must make the repeat harmless. This
// is also cheaper: (b) would need per-attempt delivery state on the sender, while the guard is one
// 10-byte record on the receiver.
//
// RAM cost: sizeof(BusRepeatEntry) = 10 B of .bss per target - one record, not a table. On the DAS
// the 10 B land in .bss as 12 B with alignment, so platformio.ini's stack_size went 1064 -> 1052
// (statics + stack = the whole 2048 B; .stack must start where .bss ends).
//
// WHERE it is asked: at the top of the two services that own the guarded CIDs
// (RegisterService::Handle for Save All, StorageService::Handle for CIDs 0-4), because each service
// already knows whether its command costs flash - a central test in DispatchPacket had to be paid
// for at every local frame and cost the DAS ~90 B more of its ~200 B of code space.
//
// A replayed repeat goes back through SendResponse, which re-records the same transaction with a
// fresh timestamp: the window slides while a retry loop is running. That is bounded by the sender's
// transmit stint (it can only retry for so long) and cannot extend protection over a DIFFERENT
// request, because that one carries a new TRID.
//
// WHY ONE RECORD AND NOT A TABLE: the ledger protects the last guarded request only. In the case it
// exists for that is enough - the sender's retry arrives within its echo window plus its own 2-33 ms
// backoff of its transmit, while the execution that retry duplicates is still running on the
// receiver (a node's Save All answered in 127-181 ms on the rig, card t_763b4cde) - so the repeat is
// sitting in the RX ring when the handler finishes and replies, and no later request can overtake
// it. The residual, stated plainly: a repeat that somehow arrives AFTER a different guarded command
// has executed is not recognised and would execute again. A second entry would cost another 12 B of
// the DAS's fully spent 2048 B (996 B statics + 1052 B stack), i.e. it would come out of the stack,
// so one entry is the deliberate choice; the key (source + TRID + CMD) plus the window make a wrong
// HIT - the worse failure, a request that never runs - impossible for anything but the same
// transaction.
//
// NOT guarded, with the verdict reached by reading each handler (t_8dead2f1 re-checked the whole
// CID table against the flash writers, not just the two services that ask the ledger):
//   * Register CID 4 (Recall All): it re-reads the same store into RAM. No record is appended.
//   * Storage CID 5 (Read): a read appends nothing; a repeat returns the same bytes. Its reply is
//     payload-bearing, so the status-only record could not replay it either.
//   * Storage CID 6 (Write, fragment): a write fragment touches FILE DATA in place, never the file
//     table, so it spends no record. Its reply carries the 2-byte sequence ack, which the
//     status-only record cannot reproduce; the stream's own check is what keeps a repeat from being
//     applied twice. NOTE: a fragment whose index is 0 always RE-OPENS the stream
//     (Storage.h), so a repeated fragment 0 rewrites the same bytes at offset 0 - harmless, but not
//     refused. Every later fragment must be exactly WriteSeq+1, so a repeat of it is refused.
//   * Register value writes and the dynamic-block CIDs (Create/Delete/GetName/SetName): the value
//     and the tombstone live in RAM; only Save All (guarded) writes `.DT_`/`.DV_`/`.SV`.
//   * Subscriptions requester CIDs (0x11 Set / 0x13 Save): they DO append `.SUBREQ`, but the writer
//     already skips the rewrite when the file holds exactly the bytes it would write
//     (SubscriptionsPersist.h SaveRequesterTable), so a duplicate execution spends no record - and
//     a cancelled entry is gone on the second pass, which writes nothing. Provider CIDs persist
//     through Save All only.
//   * Device CID 0x12 (SNDB write): it writes REGISTRY SLOTS inside an existing file (in place, and
//     only compacts - delete + create, i.e. 2 records - when the file is full), and its reply echoes
//     the 16-byte record, which the status-only record could not reproduce. A duplicate therefore
//     costs a registry slot, and costs table records only if it lands exactly when the file is full.
//     Left unguarded deliberately; recorded as a known gap rather than claimed.
//   * Format (CID 0) IS guarded: it recreates the table, so a duplicate execution is not free.
//   * Every System/Log/Script CID: no flash append at all. Device's reset/bootloader-entry CIDs are
//     disruptive rather than corrupting and are NOT covered (a known gap, not a claim).
//
// The append cost per execution, as measured by test/native/storage_repeat_test.cpp on a store in the
// DAS geometry: Resize +1 record every time it runs (so its duplicate would too), Create/Rename +1,
// Delete 0 (it invalidates the offset word in place). A duplicate Create (duplicate name) or Rename
// (old name gone) FAILS, so what a repeat costs there is a spurious FLAG_FAIL to the requester -
// which the replay fixes - while Save All and Resize are the two that append twice, and Save All is
// the expensive one at 2 records per execution.

// How long a repeat is treated as a retransmission of the last executed request.
//
// A retransmission can only arrive inside the sender's transmit stint, which is bounded by
// RS485_TX_STINT_MS (300 ms, Devices/Tamu_v2.0A/RSBus.h) plus the DAS's per-attempt echo windows.
// 400 ms therefore covers every retry the bus can produce. It also cannot swallow a NEW request
// that happens to reuse the TRID: the app allocates its TRIDs from a monotonically increasing
// 0xF000-0xFFFF counter (app/lib/core/connection.dart awardTrid), so a reuse needs 4096 further
// transactions first - seconds to minutes, never 400 ms - and the core's own System TRIDs wrap on
// the same order. The Cmd is part of the key as well, so even a reused TRID could not borrow
// another command's status.
#define BUS_REPEAT_WINDOW_MS 400

// True for the requests whose execution appends to (or rewrites) the flash file table.
// Register CID 5 = Save All (`StaticSaveAll` writes `.SV` as staging create + rename = 2 records).
// Storage CIDs 0-4 = Format, Create, Delete, Resize, Rename (each appends or invalidates records).
__attribute__((always_inline)) inline bool BusRepeatGuarded(uint16_t Cmd)
{
    const uint8_t Type = (uint8_t)(Cmd >> 8);
    const uint8_t Cid = (uint8_t)(Cmd & 0xFF);
    return (Type == (uint8_t)ServiceType::Register && Cid == 5) ||
           (Type == (uint8_t)ServiceType::Storage && Cid <= 4);
}

// The last guarded request this device executed, and what it answered.
// Kept as one record (not a table): the medium carries ONE answered unicast at a time
// (Core/Functions/BusGate.h), so the only request that can be retransmitted while another one runs
// is the one that just finished.
struct BusRepeatEntry
{
    uint16_t IdSrc;  // the requester: a retransmission comes from the same source
    uint16_t Trid;   // the transaction it belongs to
    uint16_t Cmd;    // service + CID, so a reused TRID cannot borrow another command's status
    uint16_t AtMs;   // TimeFromBoot() at execution, truncated: a wrap-safe 16-bit compare
    uint8_t Status;  // FLAG_SUCCESS or FLAG_FAIL - 0 means "nothing executed yet"
};
static_assert(sizeof(BusRepeatEntry) == 10, "the DAS has 2048 B of RAM - keep the record small");

BusRepeatEntry BusRepeatLast = {0, 0, 0, 0, 0};

// True when `Frame` is a retransmission of the last guarded request this device executed.
// `NowMs` is TimeFromBoot() (raw, monotonic - never Now(), which a time sync can step).
//
// Deliberately NOT inlined at its call sites (the two service dispatchers ask it): on the DAS the
// cost of the guard has to be one copy of this compare chain, not two - the image has ~200 B of
// code space and the record itself is already paid for in RAM.
__attribute__((noinline)) inline bool BusRepeatHit(const PacketFrame &Frame, uint16_t NowMs)
{
    if (BusRepeatLast.Status == 0)
        return false;
    if (BusRepeatLast.IdSrc != Frame.IdSrc || BusRepeatLast.Trid != Frame.Trid ||
        BusRepeatLast.Cmd != Frame.Cmd)
        return false;
    return (uint16_t)(NowMs - BusRepeatLast.AtMs) <= BUS_REPEAT_WINDOW_MS;
}


// Records the outcome of a guarded request so a repeat can be answered from it.
//
// Called from the reply builder (Core/Functions/MemoryBackup.h SendResponse) rather than from the
// dispatcher: that is the one place where the status the handler decided is still in hand, and the
// call site only exists when a reply was actually sent (SendResponse returns early without
// FLAG_REQACK), so a repeat can never be answered with a reply the original never produced.
inline void BusRepeatNote(const PacketFrame &Frame, uint8_t Status, uint16_t Len, uint16_t NowMs)
{
    if (Status == 0 || Len != 0)
        return; // only a status-only reply can be rebuilt from the record
    if (Frame.Flags & FLAG_TYPE)
        return; // a reply is not a request
    if (!BusRepeatGuarded(Frame.Cmd))
        return;
    BusRepeatLast.IdSrc = Frame.IdSrc;
    BusRepeatLast.Trid = Frame.Trid;
    BusRepeatLast.Cmd = Frame.Cmd;
    BusRepeatLast.AtMs = NowMs;
    BusRepeatLast.Status = Status;
}

// The status to replay for a repeat, for the caller that builds the answer frame.
inline uint8_t BusRepeatStatus() { return BusRepeatLast.Status; }
