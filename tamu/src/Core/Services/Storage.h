#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Storage.h"

// Sends a single no-fragment reply reusing the caller's frame (no extra PacketFrame on stack).
static void StorageReply(PacketFrame &reply, const PacketFrame &req,
                          const uint8_t *payload, uint16_t len)
{
    PacketConstruct(&reply, req.id_src, req.srv_src, req.srv_tgt,
                     FLAG_TYPE | FLAG_START | FLAG_STOP, payload, len);
    DispatchPacket(reply);
}

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

    // NOTE: no separate scratch buffer - stream fragments are packed straight into
    // tx_frame.payload (see FinalizeReply) so the DAS's 2 KB stack stays shallow.

    switch (cid) {
        case 0: { // Read File Table (FRAG stream of File entries)
            uint8_t active_count = Storage.FileCount();

            if (active_count == 0) {
                FinalizeReply(tx_frame, frame, FLAG_TYPE | FLAG_START | FLAG_STOP, 0);
                DispatchPacket(tx_frame);
                break;
            }

            // Stream every entry as a FRAG stream (Docs/Services/Storage.md:
            // "Fragmentation, File table entries"). Each fragment carries up to
            // MAX_FRAG_CONTENT_SIZE (256) bytes of 16-byte entries; the fragmentation
            // info is the first 4 payload bytes. `sent` counts what was actually read
            // so the stream always terminates with STOP even if a late entry read fails.
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
                        break; // table unreadable: stop; STOP still set below
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

        case 1: { // Format Filesystem
            Storage.Format();
            StorageReply(tx_frame, frame, nullptr, 0);
            break;
        }

        case 2: { // Create File (Request: Name (8 bytes) + Size (4 bytes))
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

        case 3: { // Delete File (Request: Name (8 bytes))
            if (PayloadBytes(frame) >= 8) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                bool ok = Storage.DeleteFile(name);
                // DeleteFile returns false only for the protected ".TABLE" entry.
                // Always answer with a status byte (consistent with Create/Resize/Rename).
                // The CLI now checks the byte; the app's StorageClient still treats any
                // reply (empty or status) as success for already-gone files, but a 0
                // status correctly surfaces table-protection failures.
                uint8_t status = ok ? 0x01 : 0x00;
                StorageReply(tx_frame, frame, &status, 1);
            }
            break;
        }

        case 4: { // Resize File (Request: Name (8 bytes) + New Size (4 bytes))
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

        case 5: { // Rename File (Request: Old Name (8 bytes) + New Name (8 bytes))
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

        case 6: { // Read File (Request: Name (8 bytes))
            // Response: stream of the whole file - fragment 0 = [frag info][name][contents],
            // later fragments = [frag info][contents]. The app knows the file size (from the
            // file table) and trims the last fragment's 4-byte wire padding.
            if (PayloadBytes(frame) >= 8) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t file_offset, file_size;
                if (Storage.GetFileInfo(name, &file_offset, &file_size)) {
                    // Chunk size = max actual payload per fragment (Data Formats.md: 256 bytes).
                    // Fragment 0 includes the 8-byte name in the Information section.
                    uint32_t total_content = file_size;
                    uint16_t contentCap = MAX_FRAG_CONTENT_SIZE;
                    uint16_t total_frags = (uint16_t)((total_content + contentCap - 1) / contentCap);
                    if (total_frags == 0) total_frags = 1; // empty file: single fragment

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
                    // File not found
                    DeviceLog("STORAGE", "read '%.8s' failed", name);
                    StorageReply(tx_frame, frame, nullptr, 0);
                }
            }
            break;
        }

        case 7: { // Write File (stream: Name, Fragmentation, File contents)
            // Request stream: fragment 0 = [frag info][name (8)][contents], later
            // fragments = [frag info][contents]. The app creates the file first (CID 2,
            // with the exact size); contents are written at offset current*MAX_FRAG_CONTENT_SIZE,
            // clamped to the file size. Each acknowledged fragment is answered with the last
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
                    uint32_t write_off = (uint32_t)frag.current * MAX_FRAG_CONTENT_SIZE;
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
                StorageReply(tx_frame, frame, ack, 2);
            }
            break;
        }

        default:
            break;
    }
}