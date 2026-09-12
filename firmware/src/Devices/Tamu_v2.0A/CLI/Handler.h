#pragma once

#include "Core/Functions/Device.h"
#include "Core/Functions/Storage.h"

// Defined in Block.h (included after this file)
void PrintField(const BlockMeta &desc, const void *data_ptr, uint16_t index, bool is_key);

// Set to true by every CLI response handler when a reply arrives. Command functions clear
// it right before dispatching and wait briefly afterwards, so a missing reply (dead
// address, service not compiled into the node) produces a timeout error instead of
// silence.
volatile bool g_cli_response_seen = false;

// Response handler: receives memory read responses from the network and prints them.
// The Register service answers with BlockInfo (Type10|Inst6|Field8|Key8) + BlockMeta +
// value, so the reply is parsed as BlockInfo, not the old BlockIndex format.
void HandleCLI_RegisterResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    // A single status byte (padded to 4 on the wire) means the operation failed
    // (RespondStatus); it is not a BlockInfo payload, so print it directly instead
    // of misparsing it as a read reply.
    if (PayloadBytes(frame) == 4)
    {
        printf("Device %d: Operation FAILED (status %d)\n", frame.id_src, frame.payload[0]);
        return;
    }
    if (PayloadBytes(frame) < 8) // BlockInfo + BlockMeta minimum
        return;

    if (frame.flags & FLAG_START)
        printf("--- Beginning Topology Dump from Device %d ---\n", frame.id_src);

    uint32_t bi = 0; memcpy(&bi, frame.payload, 4);
    const BlockMeta *meta = reinterpret_cast<const BlockMeta *>(frame.payload + 4);
    const uint8_t *data = frame.payload + 8;
    uint16_t avail = PayloadBytes(frame) - 8;

    uint16_t type = (bi >> 22) & 0x3FF;
    uint8_t inst = (bi >> 16) & 0x3F;
    uint8_t field = (bi >> 8) & 0xFF;
    uint8_t key = bi & 0xFF;

    if (type == 0 && inst == 0 && field == 0xFF)
    {
        // System block summary: [BlockInfo][BlockMeta][count]
        printf("Registry Summary [System]: %d blocks.\n", avail >= 1 ? data[0] : 0);
    }
    else if (field != 0xFF)
    {
        BlockMeta clamped = *meta;
        if (clamped.Size > avail) clamped.Size = avail;

        if (key != 0xFF)
        {
            PrintField(clamped, data, key, true);
        }
        else if (IsKeyedType((DataType)BlockMetaType(meta->FlagsAndType)))
        {
            printf("  |-- Field [%02d]: (Keyed Field, Type: 0x%03X)\n", field,
                   BlockMetaType(meta->FlagsAndType));
            uint16_t key_count = clamped.Size;
            if (key_count > avail) key_count = avail;
            if (key_count > 0)
            {
                printf("       Keys:");
                for (uint16_t i = 0; i < key_count; i++)
                    printf(" %d", data[i]);
                printf("\n");
            }
        }
        else
        {
            PrintField(clamped, data, field, false);
        }
    }
    else
    {
        // Block meta reply (field 0xFF): [BlockInfo][BlockMeta][name]
        printf("Block [%02d] (type 0x%03X) | Type: 0x%04X | Size: %d\n",
               inst, type, BlockMetaType(meta->FlagsAndType), meta->Size);
    }

    if (frame.flags & FLAG_STOP)
        printf("--- Topology Dump Complete ---\n");
}

// Response handler: prints LogHandler service replies (GetLogs record stream from a
// remote device, ClearReadLogs status).
void HandleCLI_LogResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    if (PayloadBytes(frame) == 0)
    {
        if (frame.flags & FLAG_START)
            printf("No logs on device %d.\n", frame.id_src);
        else
            printf("--- Log Dump Complete ---\n");
        return;
    }

    if (PayloadBytes(frame) == 4)
    {
        printf("Device %d: Log clear %s (status %d)\n",
               frame.id_src, frame.payload[0] == 0 ? "OK" : "FAILED", frame.payload[0]);
        return;
    }

    // GetLogs streams LogRecords as FRAG packets: the first 4 payload bytes are the
    // fragmentation info, followed by one or more 12-byte records.
    const uint8_t *data = frame.payload;
    uint16_t avail = PayloadBytes(frame);
    if (frame.flags & FLAG_FRAG)
    {
        if (avail <= 4) return;
        data += 4;
        avail -= 4;
    }

    if (avail >= sizeof(LogRecord) && (frame.flags & FLAG_START))
        printf("--- Logs from Device %d ---\n", frame.id_src);

    while (avail >= sizeof(LogRecord))
    {
        const LogRecord *r = reinterpret_cast<const LogRecord *>(data);
        const LogMessage &m = r->msg;
        printf("Dev %d | %s | Src 0x%04X | Code 0x%04X | Count %u | t=%lums\n",
               r->device_id, LogIsBlock(m) ? "Block" : "Svc",
               LogSourceId(m), LogCode(m), r->count, (unsigned long)m.timestamp);
        data += sizeof(LogRecord);
        avail -= sizeof(LogRecord);
    }
}

// Response handler: prints a single-byte status (Save/Recall/Delete results)
void HandleCLI_StatusResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    if (PayloadBytes(frame) >= 1)
    {
        uint8_t status = frame.payload[0];
        if (status == 0)
            printf("Device %d: Operation OK\n", frame.id_src);
        else
            printf("Device %d: Operation FAILED (status %d)\n", frame.id_src, status);
    }
    else
    {
        printf("Device %d: Empty status response\n", frame.id_src);
    }
}

// Response handler: prints a Create response (BlockIndex + 1 status byte).
void HandleCLI_CreateResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    if (PayloadBytes(frame) < sizeof(BlockIndex) + 1)
    {
        printf("Device %d: Invalid create response\n", frame.id_src);
        return;
    }

    const BlockIndex *idx = reinterpret_cast<const BlockIndex *>(frame.payload);
    uint8_t status = frame.payload[sizeof(BlockIndex)];

    if (idx->Block == INVALID_BLOCK || status == 0)
    {
        printf("Device %d: Create FAILED\n", frame.id_src);
        return;
    }

    printf("Device %d: Block %d created\n", frame.id_src, idx->Block);
}

// Response handler: prints Device service replies (the command is identified by
// the CID of the response's SRV SRC).
void HandleCLI_DeviceResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    uint8_t cid = GetServiceCID(frame.srv_src);

    // Current Device service CIDs (docs 00.0x): 0 Discover, 1 Ping, 2 Identify,
    // 3 TimeSync, 4 SetTimeOffset, 10 Core discover, 11-13 SNDB. Device type/SN/
    // version/capability/name/uptime/loop are Register System-block fields and are
    // answered by HandleCLI_RegisterResponse instead.
    switch (cid)
    {
        case 0: // Discover response (SN + ID)
            if (PayloadBytes(frame) >= sizeof(AssignPayload))
            {
                const AssignPayload *p = reinterpret_cast<const AssignPayload *>(frame.payload);
                char sn_str[29];
                SerialNumberToString(p->sn, sn_str, sizeof(sn_str));
                printf("Device %d: Discover -> SN %s, ID 0x%04X\n", frame.id_src, sn_str, p->new_addr);
            }
            break;

        case 1: // Ping
            printf("Device %d: PONG\n", frame.id_src);
            break;

        case 2: // Identify
            printf("Device %d: Identify sent\n", frame.id_src);
            break;

        case 3: // Time sync (t0,t1,t2) -> round-trip and offset estimate
        case 10: // Core discover (SN + uptime) - print uptime only
            if (PayloadBytes(frame) >= 12)
            {
                uint32_t t0, t1, t2;
                memcpy(&t0, frame.payload, 4);
                memcpy(&t1, frame.payload + 4, 4);
                memcpy(&t2, frame.payload + 8, 4);
                uint32_t t3 = TimeFromBoot(); // raw local time the reply was received
                // Counters wrap at 2^32 (~49.7 days); take signed deltas BEFORE widening so a
                // wrap is interpreted as a small negative interval, not a huge positive one.
                int32_t offset = (int32_t)(((int64_t)(int32_t)(t1 - t0) + (int64_t)(int32_t)(t2 - t3)) / 2);
                printf("Device %d: Time sync t0=%lu t1=%lu t2=%lu (est. offset %ld ms)\n",
                       frame.id_src, (unsigned long)t0, (unsigned long)t1, (unsigned long)t2, (long)offset);
            }
            break;

        default:
            printf("Device %d: Unknown Device response CID %u\n", frame.id_src, cid);
            break;
    }
}

// Response handler: prints Storage service replies (file table stream, file data
// stream or single-packet create/resize/delete results).
// Storage service CIDs (Docs/Services/Storage.md):
//   0 Format, 1 Create, 2 Delete, 3 Resize, 4 Rename, 5 Read, 6 Write, 7 Table
void HandleCLI_StorageResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    uint8_t cid = GetServiceCID(frame.srv_src);

    switch (cid)
    {
        case 0: // Format filesystem (03.00)
            printf("Device %d: Filesystem formatted\n", frame.id_src);
            break;

        case 1: // Create file (03.01)
            if (PayloadBytes(frame) >= 1 && frame.payload[0] != 0)
                printf("Device %d: File created\n", frame.id_src);
            else
                printf("Device %d: File create failed\n", frame.id_src);
            break;

        case 2: // Delete file (03.02)
            if (PayloadBytes(frame) >= 1 && frame.payload[0] == 0)
                printf("Device %d: File delete failed\n", frame.id_src);
            else
                printf("Device %d: File deleted\n", frame.id_src);
            break;

        case 3: // Resize file (03.03)
            if (PayloadBytes(frame) >= 1 && frame.payload[0] != 0)
                printf("Device %d: File resized\n", frame.id_src);
            else
                printf("Device %d: File resize failed\n", frame.id_src);
            break;

        case 4: // Rename file (03.04)
            if (PayloadBytes(frame) >= 1 && frame.payload[0] != 0)
                printf("Device %d: File renamed\n", frame.id_src);
            else
                printf("Device %d: File rename failed\n", frame.id_src);
            break;

        case 5: // Read file (03.05) - FRAG stream
            if (frame.flags & FLAG_START)
                printf("--- File Data from Device %d ---\n", frame.id_src);
            {
                const uint8_t *data = frame.payload;
                uint16_t avail = PayloadBytes(frame);
                if (frame.flags & FLAG_FRAG)
                {
                    if (avail <= 4) break;
                    data += 4;
                    avail -= 4;
                    if (frame.payload[0] == 0 && frame.payload[1] == 0 && avail >= 8)
                    {
                        // fragment 0: skip the echoed 8-byte name
                        data += 8;
                        avail -= 8;
                    }
                }
                for (uint16_t i = 0; i < avail; i++)
                    printf("%02X ", data[i]);
                printf("\n");
            }
            if (frame.flags & FLAG_STOP)
                printf("--- File Data Complete ---\n");
            break;

        case 6: // Write file (03.06) - single packet with last fragment index
            if (PayloadBytes(frame) >= 1)
                printf("Device %d: File write completed, last fragment %u\n", frame.id_src, frame.payload[0]);
            else
                printf("Device %d: File write failed\n", frame.id_src);
            break;

        default:
            break;
    }
}

// Response handler: prints Subscriptions service replies (CID 2 provider table,
// CID 3 requester table, CID 4 set result). Wire format matches Subscriptions.h.
void HandleCLI_SubsResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return;
    g_cli_response_seen = true;

    uint8_t cid = GetServiceCID(frame.srv_src);
    uint16_t avail = PayloadBytes(frame);

    if (cid == 4) // Set requester subscription result (1 status byte)
    {
        printf("Device %d: Subscription %s\n", frame.id_src,
               (avail >= 1 && frame.payload[0] != 0) ? "set" : "FAILED");
        return;
    }
    if (cid != 2 && cid != 3)
        return;
    if (avail < 1)
        return;

    bool requester = (cid == 3);
    uint16_t off = 1;
    uint8_t count = frame.payload[0];
    for (uint8_t i = 0; i < count && off + 4 <= avail; i++)
    {
        uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;
        if (requester)
        {
            memcpy(&a, frame.payload + off, 4); off += 4; // targetReg
            memcpy(&b, frame.payload + off, 4); off += 4; // sourceReg
            uint16_t provider = frame.payload[off] | (frame.payload[off + 1] << 8); off += 2;
            uint8_t trigger = frame.payload[off++];
            memcpy(&c, frame.payload + off, 4); off += 4; // periodMs
            memcpy(&d, frame.payload + off, 4); off += 4; // minTimeMs
            memcpy(&e, frame.payload + off, 4); off += 4; // counter
            uint8_t tol = frame.payload[off++];
            if (off + tol > avail) break;
            off += tol;
            uint16_t trid = 0;
            if (off + 2 <= avail) { memcpy(&trid, frame.payload + off, 2); off += 2; }
            printf("Device %d: ReqSub[%u] target=0x%08X source=0x%08X provider=%u trig=%u per=%ums min=%ums cnt=%u tol=%u trid=0x%04X\n",
                   frame.id_src, i, (unsigned)a, (unsigned)b, provider, trigger,
                   (unsigned)c, (unsigned)d, (unsigned)e, tol, trid);
        }
        else
        {
            memcpy(&a, frame.payload + off, 4); off += 4; // sourceReg
            uint16_t requesterAddr = frame.payload[off] | (frame.payload[off + 1] << 8); off += 2;
            uint8_t trigger = frame.payload[off++];
            memcpy(&c, frame.payload + off, 4); off += 4; // periodMs
            memcpy(&d, frame.payload + off, 4); off += 4; // lastSentMs
            memcpy(&e, frame.payload + off, 4); off += 4; // minTimeMs
            uint32_t counter = 0; memcpy(&counter, frame.payload + off, 4); off += 4;
            uint8_t tol = frame.payload[off++];
            if (off + tol > avail) break;
            off += tol;
            uint8_t lastLen = (off < avail) ? frame.payload[off] : 0; off++;
            if (off + lastLen > avail) break;
            off += lastLen;
            printf("Device %d: ProvSub[%u] source=0x%08X requester=%u trig=%u per=%ums last=%ums min=%ums cnt=%u tol=%u lastVal=%uB\n",
                   frame.id_src, i, (unsigned)a, requesterAddr, trigger,
                   (unsigned)c, (unsigned)d, (unsigned)e, (unsigned)counter, tol, lastLen);
        }
    }
}
