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
    if (!LogBuffer) return; // Heap allocation failed

    if (cid == 0)
    {
        // Inbound log report -> keep in RAM, deduplicated per device + source + code.
        if (frame.payload_len < sizeof(LogMessage))
            return;
        const LogMessage *msg = reinterpret_cast<const LogMessage *>(frame.payload);

        int free_slot = -1;
        for (int i = 0; i < MAX_LOG_RECORDS; i++)
        {
            if (LogUsed[i] &&
                LogBuffer[i].device_id == frame.id_src &&
                LogBuffer[i].msg.src_and_code == msg->src_and_code)
            {
                LogBuffer[i].count++;
                LogBuffer[i].msg.timestamp = msg->timestamp;
                return;
            }
            if (!LogUsed[i] && free_slot == -1)
                free_slot = i;
        }
        if (free_slot != -1)
        {
            LogBuffer[free_slot].device_id = frame.id_src;
            LogBuffer[free_slot].count = 1;
            LogBuffer[free_slot].msg = *msg;
            LogUsed[free_slot] = true;
        }
        return;
    }

    if (cid == 1) // GetLogs: stream every stored LogRecord entry
    {
        uint8_t active = 0;
        for (int i = 0; i < MAX_LOG_RECORDS; i++)
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

        uint8_t sent = 0;
        for (int i = 0; i < MAX_LOG_RECORDS; i++)
        {
            if (!LogUsed[i])
                continue;
            sent++;
            uint8_t flags = FLAG_TYPE;
            if (sent == 1) flags |= FLAG_START;
            if (sent == active) flags |= FLAG_STOP;

            PacketFrame reply;
            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                             flags, (const uint8_t *)&LogBuffer[i], sizeof(LogRecord));
            reply.frag_id = NextFragmentId(reply.flags);
            DispatchPacket(reply);
        }
        return;
    }

    if (cid == 2) // ClearReadLogs: clear `n` logs from the end of the buffer
    {
        uint32_t n = (frame.payload_len >= 4)
                         ? *reinterpret_cast<const uint32_t *>(frame.payload)
                         : 0;
        // Clear the last n occupied entries (highest indices are the most recently added).
        for (int i = MAX_LOG_RECORDS - 1; i >= 0 && n > 0; i--)
        {
            if (LogUsed[i])
            {
                LogUsed[i] = false;
                n--;
            }
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

