#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Storage.h"

// Handles Storage service requests (Docs/Services/Storage.md): file table/read/create/
// delete/resize/rename and the fragmented Read/Write File streams.
//
// Streams (File Table, Read File, Write File) use the FRAG flag: the first 4 payload
// bytes are the fragmentation info (u16 current fragment + u16 total fragments), the
// stream header (file name, 8 bytes) rides in the Information section of fragment 0
// only, and each fragment carries up to 256 bytes of file contents.

// Write File (CID 7) streaming context. The name arrives in fragment 0 only, so later
// fragments are associated through this single in-flight record: the app writes
// sequentially (each fragment acknowledged before the next), which makes the
// single-context model safe. Fragment 0 of a new stream replaces the context.
static char s_write_name[8] = {0};
static bool s_write_active = false;
static uint16_t s_write_seq = 0; // last contiguous fragment index written

void HandleStorageService(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    bool is_response = (frame.flags & FLAG_TYPE);

    if (is_response) return; // Storage service only processes requests

    PacketFrame reply;
    // Single scratch buffer shared by every streaming case (hoisted so the compiler
    // allocates it once instead of once per case - keeps the DAS's 2 KB stack sane).
    uint8_t buf[MAX_PAYLOAD_SIZE];

    switch (cid) {
        case 0: { // Read File Table (FRAG stream of File entries)
            uint8_t active_count = Storage.FileCount();

            if (active_count == 0) {
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(reply);
                break;
            }

            // Stream every entry as a FRAG stream (Docs/Services/Storage.md:
            // "Fragmentation, File table entries"). Each fragment carries up to 256
            // bytes of 16-byte entries; the fragmentation info is the first 4 payload
            // bytes. `sent` counts what was actually read so the stream always
            // terminates with STOP even if a late entry read fails.
            uint32_t total = (uint32_t)active_count * sizeof(FileEntry);
            uint16_t total_frags = (uint16_t)((total + 255) / 256);
            uint8_t sent = 0;
            for (uint16_t f = 0; f < total_frags && sent < active_count; f++) {
                uint8_t flags = FLAG_TYPE | FLAG_FRAG;
                if (f == 0) flags |= FLAG_START;
                WriteFragInfo(buf, f, total_frags);
                uint16_t off = 4;
                while (off - 4 + sizeof(FileEntry) <= 256 && sent < active_count) {
                    FileEntry entry;
                    if (!Storage.ReadFileEntry(sent, &entry))
                        break; // table unreadable: stop; STOP still set below
                    memcpy(buf + off, &entry, sizeof(FileEntry));
                    off += sizeof(FileEntry);
                    sent++;
                }
                if (sent >= active_count || f == total_frags - 1) flags |= FLAG_STOP;
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 flags, buf, off);
                DispatchPacket(reply);
                if (sent >= active_count) break;
            }
            break;
        }

        case 1: { // Format Filesystem
            Storage.Format();
            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                             FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
            DispatchPacket(reply);
            break;
        }

        case 2: { // Create File (Request: Name (8 bytes) + Size (4 bytes))
            if (PayloadBytes(frame) >= 12) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t size = *reinterpret_cast<const uint32_t *>(frame.payload + 8);

                bool ok = Storage.CreateFile(name, size);
                if (!ok) DeviceLog("STORAGE", "create '%.8s' size %u failed", name, (unsigned)size);
                uint8_t status = ok ? 0x01 : 0x00;
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, &status, 1);
                DispatchPacket(reply);
            } else {
                DeviceLog("STORAGE", "create short payload (%u B)", (unsigned)PayloadBytes(frame));
            }
            break;
        }

        case 3: { // Delete File (Request: Name (8 bytes))
            if (PayloadBytes(frame) >= 8) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                Storage.DeleteFile(name);

                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(reply);
            }
            break;
        }

        case 4: { // Resize File (Request: Name (8 bytes) + New Size (4 bytes))
            if (PayloadBytes(frame) >= 12) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t new_size = *reinterpret_cast<const uint32_t *>(frame.payload + 8);

                bool ok = Storage.ResizeFile(name, new_size);
                if (!ok) DeviceLog("STORAGE", "resize '%.8s' -> %u failed", name, (unsigned)new_size);
                uint8_t status = ok ? 0x01 : 0x00;
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, &status, 1);
                DispatchPacket(reply);
            } else {
                DeviceLog("STORAGE", "resize short payload (%u B)", (unsigned)PayloadBytes(frame));
            }
            break;
        }

        case 5: { // Rename File (Request: Old Name (8 bytes) + New Name (8 bytes))
            if (PayloadBytes(frame) >= 16) {
                const char *old_name = reinterpret_cast<const char *>(frame.payload);
                const char *new_name = reinterpret_cast<const char *>(frame.payload + 8);

                bool ok = Storage.RenameFile(old_name, new_name);
                if (!ok) DeviceLog("STORAGE", "rename '%.8s' -> '%.8s' failed", old_name, new_name);
                uint8_t status = ok ? 0x01 : 0x00;
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, &status, 1);
                DispatchPacket(reply);
            } else {
                DeviceLog("STORAGE", "rename short payload (%u B)", (unsigned)PayloadBytes(frame));
            }
            break;
        }

        case 6: { // Read File (Request: Name (8 bytes))
            // Response: stream of the whole file - fragment 0 = [frag info][name][contents],
            // later fragments = [frag info][contents]. The app knows the file size (from the
            // file table) and trims the last fragment's 4-byte wire padding.
            if (PayloadBytes(frame) >= 8) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t file_offset, file_size;
                if (Storage.GetFileInfo(name, &file_offset, &file_size)) {
                    uint32_t total_content = file_size;
                    uint16_t total_frags = (uint16_t)((total_content + 255) / 256);
                    if (total_frags == 0) total_frags = 1; // empty file: single fragment

                    for (uint16_t f = 0; f < total_frags; f++) {
                        uint8_t flags = FLAG_TYPE | FLAG_FRAG;
                        if (f == 0) flags |= FLAG_START;
                        if (f == total_frags - 1) flags |= FLAG_STOP;
                        WriteFragInfo(buf, f, total_frags);
                        uint16_t head = (f == 0) ? 8 : 0;
                        if (head) memcpy(buf + 4, name, 8);
                        uint32_t content_off = (uint32_t)f * 256;
                        uint16_t content_len = (total_content - content_off > 256)
                                                   ? 256
                                                   : (uint16_t)(total_content - content_off);
                        if (content_len)
                            Storage_FlashRead(file_offset + content_off, buf + 4 + head, content_len);
                        PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                         flags, buf, 4 + head + content_len);
                        DispatchPacket(reply);
                    }
                } else {
                    // File not found
                    DeviceLog("STORAGE", "read '%.8s' failed", name);
                    PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                     FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                    DispatchPacket(reply);
                }
            }
            break;
        }

        case 7: { // Write File (stream: Name, Fragmentation, File contents)
            // Request stream: fragment 0 = [frag info][name (8)][contents], later
            // fragments = [frag info][contents]. The app creates the file first (CID 2,
            // with the exact size); contents are written at offset current*256, clamped
            // to the file size. Each acknowledged fragment is answered with the last
            // sequential fragmentation index written (u16) so the app can resume after
            // a lost fragment.
            if (!(frame.flags & FLAG_FRAG)) break;
            PacketFragInfo frag = PacketGetFrag(frame);
            uint16_t plen = PayloadBytes(frame);

            const uint8_t *name = nullptr;
            const uint8_t *contents = nullptr;
            uint16_t content_len = 0;
            if (frag.current == 0) {
                if (plen < 12) break; // need at least [frag info][name]
                name = frame.payload + 4;
                contents = frame.payload + 12;
                content_len = plen - 12;
                // (Re)start the single in-flight write context.
                s_write_active = true;
                memcpy(s_write_name, name, 8);
                s_write_seq = 0xFFFF; // so current == seq + 1 holds for fragment 0
            } else {
                if (!s_write_active) break; // fragment 0 never arrived
                name = (const uint8_t *)s_write_name;
                contents = frame.payload + 4;
                content_len = plen - 4;
            }

            uint32_t file_offset, file_size;
            if (Storage.GetFileInfo((const char *)name, &file_offset, &file_size)) {
                // Write contiguously: a resend of the last index is idempotent, anything
                // ahead of it is a gap (respond with the last written index unchanged).
                if (frag.current == (uint16_t)(s_write_seq + 1)) {
                    uint32_t write_off = (uint32_t)frag.current * 256;
                    uint32_t room = (write_off < file_size) ? (file_size - write_off) : 0;
                    uint32_t chunk = content_len;
                    if (chunk > room) chunk = room;
                    if (chunk > 0) {
                        if (!Storage_FlashWrite(file_offset + write_off, contents, chunk))
                            DeviceLog("STORAGE", "write file '%.8s' flash write failed", name);
                        else
                            s_write_seq = frag.current;
                    } else {
                        s_write_seq = frag.current;
                    }
                }
            } else {
                DeviceLog("STORAGE", "write stream target '%.8s' missing", name);
            }

            if (frame.flags & FLAG_REQACK) {
                uint8_t ack[2] = {(uint8_t)(s_write_seq & 0xFF), (uint8_t)(s_write_seq >> 8)};
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, ack, 2);
                DispatchPacket(reply);
            }
            break;
        }

        default:
            break;
    }
}