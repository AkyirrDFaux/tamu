#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Log.h"
#include "Core/Functions/LogText.h"

// Log Handler service (Docs/Services/Log Handler.md):
//   CID 0: Error reporter (outbound on nodes, inbound DB input on the core).
//   CID 1: GetLogs - stream of LogRecord entries (core only).
//   CID 2: ClearReadLogs - clear N logs from the end, confirm (core only).
//   CID 3: GetTextLogs - the device's own free-text lines, out of the bounded RAM ring
//          (Core/Functions/LogText.h, Issues.md E70): request payload = the sequence number to
//          start AFTER (u32, absent = everything retained), reply = one line per fragment.
//   CID 4: GetTextLogState - the ring's counters (16 B, never fragmented). Core only.
//
// CIDs 3 and 4 are core-only because only the core keeps the ring: a node's log carries no free
// text (`DEVICE_LOG_TEXTLESS`, src/Main.cpp) and the 2 KB DAS has no room for it. They are the
// reason the device's lines are readable at all while a host holds the USB port - the console copy
// is suppressed then, and a line a WRITE triggers is produced exactly in that window.
class LogHandlerService
{
public:
    bool Handle(const PacketFrame &Frame); // CID dispatch; true if consumed

private:
#ifdef TYPE_CORE
    // qsort comparator: orders used-slot indices by their LogSeq (ascending = oldest first).
    // Reads the shared LogSeq table, so it is only valid on the single task that runs the
    // handler (no concurrent log append).
    static int LogSeqSlotCompare(const void *Left, const void *Right);

    // Fills `order` (at least LOG_MAX_CAPACITY entries) with the used slot indices sorted
    // oldest-first by LogSeq and returns how many were written. One ordered pass feeds both
    // GetLogs and ClearReadLogs.
    static uint32_t LogBuildOrder(uint16_t *Order);

    uint32_t Clock = 0;               // monotonic sequence source for "oldest" tracking
    uint16_t Order[LOG_MAX_CAPACITY];
#endif
};

// One instance per binary, in Core/Services/.
LogHandlerService LogHandler;

#ifdef TYPE_CORE
// The core's own text ring (Core/Functions/LogText.h): every `DeviceLog` line is appended here
// whether or not the console copy can be written, and CIDs 3/4 below read it back. Static RAM:
// 32 slots * 112 B = 3,584 B, plus 24 B of counters - the only RAM this feature costs, and it
// cannot grow (a full ring overwrites the oldest line and counts it). Core only: a node's log has
// no free text, and the DAS is at 2,048/2,048 B.
LogTextRing LogText;

// A text line is emitted whole into ONE fragment: the record has to fit MAX_FRAG_CONTENT_SIZE, or
// a line could be cut across fragments and a lost fragment would leave half a line on the host.
static_assert(LOG_TEXT_RECORD_MAX <= MAX_FRAG_CONTENT_SIZE,
              "a log line must fit one fragment (LogText.h geometry vs Packet.h frag capacity)");
#endif

#ifdef TYPE_CORE
inline int LogHandlerService::LogSeqSlotCompare(const void *Left, const void *Right)
{
    uint16_t Ia = *reinterpret_cast<const uint16_t *>(Left);
    uint16_t Ib = *reinterpret_cast<const uint16_t *>(Right);
    uint32_t Sa = LogSeq[Ia];
    uint32_t Sb = LogSeq[Ib];
    return (Sa > Sb) - (Sa < Sb);
}

inline uint32_t LogHandlerService::LogBuildOrder(uint16_t *Order)
{
    uint32_t Used = 0;
    for (uint32_t i = 0; i < LogCount; i++)
        if (LogUsed[i])
            Order[Used++] = (uint16_t)i;
    if (Used > 1)
        qsort(Order, Used, sizeof(uint16_t), LogSeqSlotCompare);
    return Used;
}
#endif

inline bool LogHandlerService::Handle(const PacketFrame &Frame)
{
    uint8_t Cid = GetServiceCID(Frame.Cmd);
    if (Frame.Flags & FLAG_TYPE) return true; // Ignore responses

#ifndef TYPE_CORE
    // Nodes are outbound-only log reporters; they do not process incoming reports.
    (void)Cid;
    return true;
#else
    EnsureLogStorage();
    if (!LogBuffer || !LogUsed || !LogSeq)
    {
        DeviceLog("LOGH", "log DB allocation failed"); // Heap allocation failed
        return true;
    }

    if (Cid == 0)
    {
        // Inbound log report -> keep in RAM, deduplicated per device + source + code.
        if (PayloadBytes(Frame) < sizeof(LogMessage))
            return true;
        const LogMessage *Msg = reinterpret_cast<const LogMessage *>(Frame.Payload);
        uint32_t Seq = ++Clock;

        int FreeSlot = -1;
        int OldestSlot = -1;
        uint32_t OldestSeq = 0xFFFFFFFF;
        for (uint32_t i = 0; i < LogCapacity; i++)
        {
            if (i < LogCount && LogUsed[i])
            {
                if (LogBuffer[i].DeviceId == Frame.IdSrc &&
                    LogBuffer[i].Msg.Source == Msg->Source &&
                    LogCode(LogBuffer[i].Msg) == LogCode(*Msg))
                {
                    // Dedup hit: refresh in place, newest wins the sequence number.
                    LogBuffer[i].Count++;
                    LogBuffer[i].Msg.Timestamp = Msg->Timestamp;
                    LogSeq[i] = Seq;
                    return true;
                }
                if (LogSeq[i] < OldestSeq)
                {
                    OldestSeq = LogSeq[i];
                    OldestSlot = (int)i;
                }
            }
            else if (FreeSlot == -1)
                FreeSlot = (int)i; // any unused slot: tail or a cleared-by-ClearReadLogs hole
        }

        uint32_t Slot;
        if (FreeSlot >= 0)
        {
            Slot = (uint32_t)FreeSlot;
        }
        else
        {
            // Database full: try to GROW it on the heap first; only when the heap cannot
            // provide more room, drop the OLDEST record to make space for this one.
            if (LogCapacity < LOG_MAX_CAPACITY && GrowLogStorage(LogCapacity + LOG_GROW_STEP))
                Slot = LogCount++;
            else
                Slot = (uint32_t)OldestSlot; // growth failed / hard cap: drop the oldest record
        }

        LogBuffer[Slot].DeviceId = Frame.IdSrc;
        LogBuffer[Slot].Count = 1;
        LogBuffer[Slot].Msg = *Msg;
        LogUsed[Slot] = true;
        LogSeq[Slot] = Seq;
        if ((uint32_t)Slot >= LogCount) LogCount = Slot + 1;
        return true;
    }

    if (Cid == 3)
    {
        // GetTextLogs (Issues.md E70): the device's own free-text lines out of the bounded ring.
        // The request's payload is the sequence number to start AFTER, so a reader resumes where it
        // left off; no payload means "everything still retained", which is what a fresh session
        // asks for. The ring's counters are counted from in the same lock, so the two passes below
        // walk one consistent window.
        uint32_t After = 0;
        if (PayloadBytes(Frame) >= 4)
            memcpy(&After, Frame.Payload, sizeof(After));

        LogTextView View;
        LogTextLock();
        View = LogTextViewOf(LogText);
        LogTextUnlock();

        // Pass 1: how many lines will be emitted. A line that cannot be encoded (it cannot with
        // this geometry - the static_assert pins it) is not counted, so the fragment count the host
        // reassembles by is always the number of fragments that follow.
        uint8_t Content[MAX_FRAG_CONTENT_SIZE];
        uint16_t Total = 0;
        for (uint32_t k = 0; k < View.Count; k++)
        {
            LogTextLock();
            const LogTextSlot &Slot = LogTextSlotAt(LogText, View, k);
            uint16_t Len = (Slot.Seq > After) ? LogTextEncodeLine(Slot, Content, sizeof(Content)) : 0;
            LogTextUnlock();
            if (Len)
                Total++;
        }
        if (Total == 0)
        {
            SendResponse(Frame, nullptr, 0); // nothing new: the ring is empty or fully read
            return true;
        }

        // Pass 2: one line per fragment. A writer overwriting a slot between the two passes can
        // add a line past the count above (it is dropped here and picked up by the next read) or
        // replace one the walk had not copied yet; either way a fragment holds a WHOLE line copied
        // under the lock, and a short stream is visible to the host as fragments < total.
        uint16_t Frag = 0;
        for (uint32_t k = 0; k < View.Count && Frag < Total; k++)
        {
            LogTextLock();
            const LogTextSlot &Slot = LogTextSlotAt(LogText, View, k);
            uint16_t Len = (Slot.Seq > After) ? LogTextEncodeLine(Slot, Content, sizeof(Content)) : 0;
            LogTextUnlock();
            if (!Len)
                continue;
            memcpy(TxFrame.Payload + 4, Content, Len);
            SendFragFragment(Frame, Frag, Total, Len, PRIORITY_LOG);
            Frag++;
        }
        return true;
    }

    if (Cid == 4)
    {
        // GetTextLogState: the ring's counters, 16 B and never fragmented, so a reader can tell an
        // empty ring from a quiet device and can see how many lines a full ring overwrote
        // (Issues.md E70 acceptance: "a full ring overwrites the oldest line and counts it").
        LogTextLock();
        LogTextView View = LogTextViewOf(LogText);
        LogTextUnlock();
        uint8_t State[LOG_TEXT_STATE_SIZE];
        LogTextEncodeState(View, State);
        SendResponse(Frame, State, (uint16_t)sizeof(State));
        return true;
    }

    if (Cid == 1 || Cid == 2)
    {
        // One ordered view of the database (oldest first by LogSeq) feeds both CIDs:
        // GetLogs must emit oldest-first, and ClearReadLogs must drop the lowest sequences.
        uint32_t Active = LogBuildOrder(Order);

        if (Cid == 1) // GetLogs: stream every stored LogRecord entry, oldest first
        {
            if (Active == 0)
            {
                SendResponse(Frame, nullptr, 0);
                return true;
            }

            // Stream every record as a FRAG stream (Docs/Services/Log Handler.md:
            // "Fragmentation, entries"). Each fragment carries up to 112 bytes of
            // LogRecords; SendFragFragment writes the 4-byte frag info.
            uint32_t Total = Active * sizeof(LogRecord);
            uint16_t TotalFrags = (uint16_t)((Total + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
            uint32_t Sent = 0;
            for (uint16_t Frag = 0; Frag < TotalFrags; Frag++)
            {
                uint16_t Off = 4;
                while (Off - 4 + sizeof(LogRecord) <= MAX_FRAG_CONTENT_SIZE && Sent < Active)
                {
                    memcpy(TxFrame.Payload + Off, &LogBuffer[Order[Sent]], sizeof(LogRecord));
                    Off += sizeof(LogRecord);
                    Sent++;
                }
                SendFragFragment(Frame, Frag, TotalFrags, (uint16_t)(Off - 4), PRIORITY_LOG);
            }
            return true;
        }

        // ClearReadLogs: clear the `n` OLDEST entries (lowest seq), per Log Handler.md
        uint32_t Count = 0;
        if (PayloadBytes(Frame) >= 4)
            memcpy(&Count, Frame.Payload, sizeof(Count));
        uint32_t Clear = (Count < Active) ? Count : Active;
        for (uint32_t k = 0; k < Clear; k++)
            LogUsed[Order[k]] = false;
        uint8_t Status = 0;
        SendResponse(Frame, &Status, 1);
    }
    return true;
#endif
}
