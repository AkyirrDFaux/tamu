#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Log.h"

// Log Handler service (Docs/Services/Log Handler.md):
//   CID 0: Error reporter (outbound on nodes, inbound DB input on the core).
//   CID 1: GetLogs - stream of LogRecord entries (core only).
//   CID 2: ClearReadLogs - clear N logs from the end, confirm (core only).
class LogHandlerService
{
public:
    void Init();
    bool Handle(const PacketFrame &Frame); // CID dispatch; true if consumed

private:
#ifdef TYPE_CORE
    // qsort comparator: orders used-slot indices by their LogSeq (ascending = oldest first).
    // Reads the shared LogSeq table, so it is only valid on the single task that runs the
    // handler (no concurrent log append).
    static int LogSeqSlotCompare(const void *a, const void *b);

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

inline void LogHandlerService::Init()
{
#ifdef TYPE_CORE
    Clock = 0;
    memset(Order, 0, sizeof(Order));
#endif
}

#ifdef TYPE_CORE
inline int LogHandlerService::LogSeqSlotCompare(const void *a, const void *b)
{
    uint16_t Ia = *reinterpret_cast<const uint16_t *>(a);
    uint16_t Ib = *reinterpret_cast<const uint16_t *>(b);
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
            for (uint16_t f = 0; f < TotalFrags; f++)
            {
                uint16_t Off = 4;
                while (Off - 4 + sizeof(LogRecord) <= MAX_FRAG_CONTENT_SIZE && Sent < Active)
                {
                    memcpy(TxFrame.Payload + Off, &LogBuffer[Order[Sent]], sizeof(LogRecord));
                    Off += sizeof(LogRecord);
                    Sent++;
                }
                SendFragFragment(Frame, f, TotalFrags, (uint16_t)(Off - 4), PRIORITY_LOG);
            }
            return true;
        }

        // ClearReadLogs: clear the `n` OLDEST entries (lowest seq), per Log Handler.md
        uint32_t n = 0;
        if (PayloadBytes(Frame) >= 4)
            memcpy(&n, Frame.Payload, sizeof(n));
        uint32_t Clear = (n < Active) ? n : Active;
        for (uint32_t k = 0; k < Clear; k++)
            LogUsed[Order[k]] = false;
        uint8_t Status = 0;
        SendResponse(Frame, &Status, 1);
    }
    return true;
#endif
}
