#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Log.h"

// Log Handler service (Docs/Services/Log Handler.md):
//   CID 0: Error reporter (outbound on nodes, inbound DB input on the core).
//   CID 1: GetLogs - stream of LogRecord entries (core only).
//   CID 2: ClearReadLogs - clear N logs from the end, confirm (core only).
void HandleLogHandler(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    if (frame.flags & FLAG_TYPE) return; // Ignore responses

#ifndef TYPE_CORE
    // Nodes are outbound-only log reporters; they do not process incoming reports.
    (void)cid;
#else
    EnsureLogStorage();
    if (!LogBuffer || !LogUsed || !LogSeq) { DeviceLog("LOGH", "log DB allocation failed"); return; } // Heap allocation failed

    static uint32_t log_clock = 0; // monotonic sequence source for "oldest" tracking

    if (cid == 0)
    {
        // Inbound log report -> keep in RAM, deduplicated per device + source + code.
        if (PayloadBytes(frame) < sizeof(LogMessage))
            return;
        const LogMessage *msg = reinterpret_cast<const LogMessage *>(frame.payload);
        uint32_t seq = ++log_clock;

        int free_slot = -1;
        int oldest_slot = -1;
        uint32_t oldest_seq = 0xFFFFFFFF;
        for (uint32_t i = 0; i < LogCapacity; i++)
        {
            if (i < LogCount && LogUsed[i])
            {
                if (LogBuffer[i].device_id == frame.id_src &&
                    LogBuffer[i].msg.src_and_code == msg->src_and_code)
                {
                    // Dedup hit: refresh in place, newest wins the sequence number.
                    LogBuffer[i].count++;
                    LogBuffer[i].msg.timestamp = msg->timestamp;
                    LogSeq[i] = seq;
                    return;
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
            if (LogCapacity < LOG_MAX_CAPACITY)
            {
                LogRecord *nb = (LogRecord *)realloc(LogBuffer, (LogCapacity + LOG_GROW_STEP) * sizeof(LogRecord));
                bool *nu = (bool *)realloc(LogUsed, (LogCapacity + LOG_GROW_STEP) * sizeof(bool));
                uint32_t *ns = (uint32_t *)realloc(LogSeq, (LogCapacity + LOG_GROW_STEP) * sizeof(uint32_t));
                if (nb && nu && ns)
                {
                    memset(nu + LogCapacity, 0, LOG_GROW_STEP * sizeof(bool));
                    LogBuffer = nb; LogUsed = nu; LogSeq = ns;
                    LogCapacity += LOG_GROW_STEP;
                    slot = LogCount++;
                }
                else
                {
                    free(nb); free(nu); free(ns); // partial failure: keep the old buffers
                    slot = (uint32_t)oldest_slot; // drop the oldest record
                }
            }
            else
            {
                slot = (uint32_t)oldest_slot; // hard cap reached: drop the oldest record
            }
        }

        LogBuffer[slot].device_id = frame.id_src;
        LogBuffer[slot].count = 1;
        LogBuffer[slot].msg = *msg;
        LogUsed[slot] = true;
        LogSeq[slot] = seq;
        if ((uint32_t)slot >= LogCount) LogCount = slot + 1;
        return;
    }

    if (cid == 1) // GetLogs: stream every stored LogRecord entry
    {
        uint8_t active = 0;
        for (uint32_t i = 0; i < LogCount; i++)
            if (LogUsed[i])
                active++;

        if (active == 0)
        {
            PacketFrame reply;
            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                             FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
            DispatchPacket(reply);
            return;
        }

        // Stream every record as a FRAG stream (Docs/Services/Log Handler.md:
        // "Fragmentation, entries"). Each fragment carries up to 256 bytes of
        // LogRecords; the fragmentation info is the first 4 payload bytes.
        uint32_t total = (uint32_t)active * sizeof(LogRecord);
        uint16_t total_frags = (uint16_t)((total + 255) / 256);
        uint8_t buf[MAX_PAYLOAD_SIZE];
        uint8_t sent = 0;
        uint32_t scan = 0;
        for (uint16_t f = 0; f < total_frags && sent < active; f++)
        {
            uint8_t flags = FLAG_TYPE | FLAG_FRAG;
            if (f == 0) flags |= FLAG_START;
            WriteFragInfo(buf, f, total_frags);
            uint16_t off = 4;
            while (off - 4 + sizeof(LogRecord) <= 256 && sent < active)
            {
                while (scan < LogCount && !LogUsed[scan]) scan++;
                if (scan >= LogCount) break;
                memcpy(buf + off, &LogBuffer[scan], sizeof(LogRecord));
                off += sizeof(LogRecord);
                scan++;
                sent++;
            }
            if (sent >= active || f == total_frags - 1) flags |= FLAG_STOP;

            PacketFrame reply;
            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                             flags, buf, off);
            DispatchPacket(reply);
            if (sent >= active) break;
        }
        return;
    }

    if (cid == 2) // ClearReadLogs: clear the `n` most recently received logs
    {
        uint32_t n = (PayloadBytes(frame) >= 4)
                         ? *reinterpret_cast<const uint32_t *>(frame.payload)
                         : 0;
        // Clear the n entries with the HIGHEST sequence number (the newest). Slot order
        // no longer tracks age once evictions/compaction have scrambled it, so selection
        // goes by LogSeq.
        while (n > 0)
        {
            int newest = -1;
            uint32_t newest_seq = 0;
            for (uint32_t i = 0; i < LogCount; i++)
            {
                if (LogUsed[i] && LogSeq[i] > newest_seq)
                {
                    newest_seq = LogSeq[i];
                    newest = (int)i;
                }
            }
            if (newest < 0)
                break; // database empty
            LogUsed[newest] = false;
            n--;
        }
        if (frame.flags & FLAG_REQACK)
        {
            uint8_t status = 0;
            PacketFrame reply;
            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                             FLAG_TYPE | FLAG_START | FLAG_STOP, &status, 1);
            DispatchPacket(reply);
        }
    }
#endif
}

