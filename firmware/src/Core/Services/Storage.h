#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Storage.h"

static void StorageReply(PacketFrame &reply, const PacketFrame &req,
                           const uint8_t *payload, uint16_t len)
{
    PacketConstruct(&reply, req.id_src, req.srv_src, req.srv_tgt,
                     FLAG_TYPE | FLAG_START | FLAG_STOP, payload, len);
    DispatchPacket(reply);
}

static char s_write_name[8] = {0};
static bool s_write_active = false;
static uint16_t s_write_seq = 0;
static uint32_t s_write_off = 0;

void HandleStorageService(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    bool is_response = (frame.flags & FLAG_TYPE);

    if (is_response) return;

    switch (cid) {
        case 0: { // Format per docs 03.00
            Storage.Format();
            StorageReply(tx_frame, frame, nullptr, 0);
            break;
        }

        case 1: { // Create per docs 03.01
            if (PayloadBytes(frame) >= 12) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t size; memcpy(&size, frame.payload + 8, sizeof(size));
                bool ok = Storage.CreateFile(name, size);
                if (!ok) DeviceLog("STORAGE", "create '%.8s' size %u failed", name, (unsigned)size);
                uint8_t status = ok ? 0x01 : 0x00;
                StorageReply(tx_frame, frame, &status, 1);
            } else {
                DeviceLog("STORAGE", "create short payload (%u B)", (unsigned)PayloadBytes(frame));
            }
            break;
        }

        case 2: { // Delete per docs 03.02
            if (PayloadBytes(frame) >= 8) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                bool ok = Storage.DeleteFile(name);
                uint8_t status = ok ? 0x01 : 0x00;
                StorageReply(tx_frame, frame, &status, 1);
            }
            break;
        }

        case 3: { // Resize per docs 03.03
            if (PayloadBytes(frame) >= 12) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t new_size; memcpy(&new_size, frame.payload + 8, sizeof(new_size));
                bool ok = Storage.ResizeFile(name, new_size);
                if (!ok) DeviceLog("STORAGE", "resize '%.8s' -> %u failed", name, (unsigned)new_size);
                uint8_t status = ok ? 0x01 : 0x00;
                StorageReply(tx_frame, frame, &status, 1);
            } else {
                DeviceLog("STORAGE", "resize short payload (%u B)", (unsigned)PayloadBytes(frame));
            }
            break;
        }

        case 4: { // Rename per docs 03.04
            if (PayloadBytes(frame) >= 16) {
                const char *old_name = reinterpret_cast<const char *>(frame.payload);
                const char *new_name = reinterpret_cast<const char *>(frame.payload + 8);
                bool ok = Storage.RenameFile(old_name, new_name);
                if (!ok) DeviceLog("STORAGE", "rename '%.8s' -> '%.8s' failed", old_name, new_name);
                uint8_t status = ok ? 0x01 : 0x00;
                StorageReply(tx_frame, frame, &status, 1);
            } else {
                DeviceLog("STORAGE", "rename short payload (%u B)", (unsigned)PayloadBytes(frame));
            }
            break;
        }

        case 5: { // Read per docs 03.05
            if (PayloadBytes(frame) >= 8) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t file_offset, file_size;
                if (Storage.GetFileInfo(name, &file_offset, &file_size)) {
                    uint32_t total_content = file_size;
                    // Fragment 0 has 4 frag_info + 8 name = 12 overhead, leaving 104 for content.
                    // Other fragments have 4 overhead, leaving 112. Use 104 for all to simplify.
                    uint16_t contentCap = MAX_FRAG_CONTENT_SIZE - 8;
                    uint16_t total_frags = (uint16_t)((total_content + contentCap - 1) / contentCap);
                    if (total_frags == 0) total_frags = 1;
                    for (uint16_t f = 0; f < total_frags; f++) {
                        uint8_t flags = FLAG_TYPE | FLAG_FRAG;
                        if (f == 0) flags |= FLAG_START;
                        if (f == total_frags - 1) flags |= FLAG_STOP;
                        WriteFragInfo(tx_frame.payload, f, total_frags);
                        uint16_t head = (f == 0) ? 8 : 0;
                        if (head) memcpy(tx_frame.payload + 4, name, 8);
                        uint32_t content_off = (uint32_t)f * contentCap;
                        uint16_t content_len = (total_content - content_off > contentCap)
                                                   ? contentCap
                                                   : (uint16_t)(total_content - content_off);
                        if (content_len)
                            Storage_FlashRead(file_offset + content_off, tx_frame.payload + 4 + head, content_len);
                        FinalizeReply(tx_frame, frame, flags, (uint16_t)(4 + head + content_len));
                        DispatchPacket(tx_frame);
                    }
                } else {
                    DeviceLog("STORAGE", "read '%.8s' failed", name);
                    StorageReply(tx_frame, frame, nullptr, 0);
                }
            }
            break;
        }

        case 6: { // Write per docs 03.06
            if (!(frame.flags & FLAG_FRAG)) break;
            PacketFragInfo frag = PacketGetFrag(frame);
            uint16_t plen = PayloadBytes(frame);
            const uint8_t *name = nullptr;
            const uint8_t *contents = nullptr;
            uint16_t content_len = 0;
            if (frag.current == 0) {
                if (plen < 12) break;
                name = frame.payload + 4;
                contents = frame.payload + 12;
                content_len = plen - 12;
                s_write_active = true;
                memcpy(s_write_name, name, 8);
                s_write_seq = 0xFFFF;
                s_write_off = 0;
            } else {
                if (!s_write_active) break;
                name = (const uint8_t *)s_write_name;
                contents = frame.payload + 4;
                content_len = plen - 4;
            }
            uint32_t file_offset, file_size;
            if (Storage.GetFileInfo((const char *)name, &file_offset, &file_size)) {
                if (frag.current == (uint16_t)(s_write_seq + 1)) {
                    uint32_t room = (s_write_off < file_size) ? (file_size - s_write_off) : 0;
                    uint32_t chunk = content_len;
                    if (chunk > room) chunk = room;
                    if (chunk > 0) {
                        if (!Storage_FlashWrite(file_offset + s_write_off, contents, chunk))
                            DeviceLog("STORAGE", "write file '%.8s' flash write failed", name);
                        else {
                            s_write_off += chunk;
                            s_write_seq = frag.current;
                        }
                    } else {
                        s_write_seq = frag.current;
                    }
                }
            } else {
                DeviceLog("STORAGE", "write stream target '%.8s' missing", name);
            }
            if (frame.flags & FLAG_REQACK) {
                uint8_t ack[2] = {(uint8_t)(s_write_seq & 0xFF), (uint8_t)(s_write_seq >> 8)};
                StorageReply(tx_frame, frame, ack, 2);
            }
            break;
        }

        case 7: { // Extra: Read File Table (not in docs, for app)
            uint8_t active_count = Storage.FileCount();
            if (active_count == 0) {
                FinalizeReply(tx_frame, frame, FLAG_TYPE | FLAG_START | FLAG_STOP, 0);
                DispatchPacket(tx_frame);
                break;
            }
            uint32_t total = (uint32_t)active_count * sizeof(FileEntry);
            uint16_t frag_content_cap = MAX_FRAG_CONTENT_SIZE;
            uint16_t total_frags = (uint16_t)((total + frag_content_cap - 1) / frag_content_cap);
            uint8_t sent = 0;
            for (uint16_t f = 0; f < total_frags && sent < active_count; f++) {
                uint8_t flags = FLAG_TYPE | FLAG_FRAG;
                if (f == 0) flags |= FLAG_START;
                WriteFragInfo(tx_frame.payload, f, total_frags);
                uint16_t off = 4;
                while (off + sizeof(FileEntry) <= (uint16_t)(4 + frag_content_cap) && sent < active_count) {
                    FileEntry entry;
                    if (!Storage.ReadFileEntry(sent, &entry))
                        break;
                    memcpy(tx_frame.payload + off, &entry, sizeof(FileEntry));
                    off += sizeof(FileEntry);
                    sent++;
                }
                if (sent >= active_count || f == total_frags - 1) flags |= FLAG_STOP;
                FinalizeReply(tx_frame, frame, flags, off);
                DispatchPacket(tx_frame);
                if (sent >= active_count) break;
            }
            break;
        }

        default:
            break;
    }
}
