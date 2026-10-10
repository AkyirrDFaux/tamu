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
    bool Handle(const PacketFrame &frame); // CID dispatch; true if consumed

private:
#ifdef TYPE_CORE
    // qsort comparator: orders used-slot indices by their LogSeq (ascending = oldest first).
    // Reads the shared LogSeq table, so it is only valid on the single task that runs the
    // handler (no concurrent log append).
    static int LogSeqSlotCompare(const void *a, const void *b);

    // Fills `order` (at least LOG_MAX_CAPACITY entries) with the used slot indices sorted
    // oldest-first by LogSeq and returns how many were written. One ordered pass feeds both
    // GetLogs and ClearReadLogs.
    static uint32_t LogBuildOrder(uint16_t *order);

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
    uint16_t ia = *reinterpret_cast<const uint16_t *>(a);
    uint16_t ib = *reinterpret_cast<const uint16_t *>(b);
    uint32_t sa = LogSeq[ia];
    uint32_t sb = LogSeq[ib];
    return (sa > sb) - (sa < sb);
}

inline uint32_t LogHandlerService::LogBuildOrder(uint16_t *order)
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < LogCount; i++)
        if (LogUsed[i])
            order[used++] = (uint16_t)i;
    if (used > 1)
        qsort(order, used, sizeof(uint16_t), LogSeqSlotCompare);
    return used;
}
#endif

inline bool LogHandlerService::Handle(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.Cmd);
    if (frame.Flags & FLAG_TYPE) return true; // Ignore responses

#ifndef TYPE_CORE
    // Nodes are outbound-only log reporters; they do not process incoming reports.
    (void)cid;
    return true;
#else
    EnsureLogStorage();
    if (!LogBuffer || !LogUsed || !LogSeq)
    {
        DeviceLog("LOGH", "log DB allocation failed"); // Heap allocation failed
        return true;
    }

    if (cid == 0)
    {
        // Inbound log report -> keep in RAM, deduplicated per device + source + code.
        if (PayloadBytes(frame) < sizeof(LogMessage))
            return true;
        const LogMessage *msg = reinterpret_cast<const LogMessage *>(frame.Payload);
        uint32_t seq = ++Clock;

        int free_slot = -1;
        int oldest_slot = -1;
        uint32_t oldest_seq = 0xFFFFFFFF;
        for (uint32_t i = 0; i < LogCapacity; i++)
        {
            if (i < LogCount && LogUsed[i])
            {
                if (LogBuffer[i].device_id == frame.IdSrc &&
                    LogBuffer[i].msg.source == msg->source &&
                    LogCode(LogBuffer[i].msg) == LogCode(*msg))
                {
                    // Dedup hit: refresh in place, newest wins the sequence number.
                    LogBuffer[i].count++;
                    LogBuffer[i].msg.timestamp = msg->timestamp;
                    LogSeq[i] = seq;
                    return true;
                }
                if (LogSeq[i] < oldest_seq)
                {
                    oldest_seq = LogSeq[i];
                    oldest_slot = (int)i;
                }
            }
            else if (free_slot == -1)
                free_slot = (int)i; // any unused slot: tail or a cleared-by-ClearReadLogs hole
        }

        uint32_t slot;
        if (free_slot >= 0)
        {
            slot = (uint32_t)free_slot;
        }
        else
        {
            // Database full: try to GROW it on the heap first; only when the heap cannot
            // provide more room, drop the OLDEST record to make space for this one.
            if (LogCapacity < LOG_MAX_CAPACITY && GrowLogStorage(LogCapacity + LOG_GROW_STEP))
                slot = LogCount++;
            else
                slot = (uint32_t)oldest_slot; // growth failed / hard cap: drop the oldest record
        }

        LogBuffer[slot].device_id = frame.IdSrc;
        LogBuffer[slot].count = 1;
        LogBuffer[slot].msg = *msg;
        LogUsed[slot] = true;
        LogSeq[slot] = seq;
        if ((uint32_t)slot >= LogCount) LogCount = slot + 1;
        return true;
    }

    if (cid == 1 || cid == 2)
    {
        // One ordered view of the database (oldest first by LogSeq) feeds both CIDs:
        // GetLogs must emit oldest-first, and ClearReadLogs must drop the lowest sequences.
        uint32_t active = LogBuildOrder(Order);

        if (cid == 1) // GetLogs: stream every stored LogRecord entry, oldest first
        {
            if (active == 0)
            {
                SendResponse(frame, nullptr, 0);
                return true;
            }

            // Stream every record as a FRAG stream (Docs/Services/Log Handler.md:
            // "Fragmentation, entries"). Each fragment carries up to 112 bytes of
            // LogRecords; SendFragFragment writes the 4-byte frag info.
            uint32_t Total = active * sizeof(LogRecord);
            uint16_t total_frags = (uint16_t)((Total + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
            uint32_t sent = 0;
            for (uint16_t f = 0; f < total_frags; f++)
            {
                uint16_t off = 4;
                while (off - 4 + sizeof(LogRecord) <= MAX_FRAG_CONTENT_SIZE && sent < active)
                {
                    memcpy(TxFrame.Payload + off, &LogBuffer[Order[sent]], sizeof(LogRecord));
                    off += sizeof(LogRecord);
                    sent++;
                }
                SendFragFragment(frame, f, total_frags, (uint16_t)(off - 4), PRIORITY_LOG);
            }
            return true;
        }

        // ClearReadLogs: clear the `n` OLDEST entries (lowest seq), per Log Handler.md
        uint32_t n = 0;
        if (PayloadBytes(frame) >= 4)
            memcpy(&n, frame.Payload, sizeof(n));
        uint32_t clear = (n < active) ? n : active;
        for (uint32_t k = 0; k < clear; k++)
            LogUsed[Order[k]] = false;
        uint8_t status = 0;
        SendResponse(frame, &status, 1);
    }
    return true;
#endif
}
