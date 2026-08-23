#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Storage.h"

// Handles Storage service requests: file table/read/create/delete/resize and stream open/close/writes.
void HandleStorageService(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    bool is_response = (frame.flags & FLAG_TYPE);

    if (is_response) return; // Storage service only processes requests

    PacketFrame reply;

    if (cid >= 64) {
        // --- Write Stream ---
        uint8_t stream_idx = cid - 64;
        if (stream_idx < MAX_STREAMS && Storage.streams[stream_idx].active) {
            WriteStream &stream = Storage.streams[stream_idx];
            // File info is cached at stream open (offset/size); a full table walk per
            // packet would cost O(table) flash reads on every single write chunk.
            uint32_t file_offset = stream.file_offset;
            uint32_t file_size = stream.file_size;
            {
                // Clamp the write so a stream can never overwrite flash beyond the file
                // (the file table or an adjacent file).
                if (stream.current_offset < file_size) {
                    uint32_t chunk_len = frame.payload_len;
                    if (stream.current_offset + chunk_len > file_size) chunk_len = file_size - stream.current_offset;
                    Storage_FlashWrite(file_offset + stream.current_offset, frame.payload, chunk_len);
                    stream.current_offset += chunk_len;
                    if (stream.current_offset >= file_size)
                        stream.active = false; // file complete: stop accepting packets
                }
            }
        }
        return;
    }

    switch (cid) {
        case 0: { // Read File Table (Stream response of File entries)
            uint8_t active_count = Storage.FileCount();

            if (active_count == 0) {
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(reply);
                break;
            }

            uint8_t sent = 0;
            for (uint8_t index = 0; index < active_count; index++) {
                FileEntry entry;
                if (!Storage.ReadFileEntry(index, &entry))
                    break; // table unreadable: stop; the final packet below still carries FLAG_STOP
                sent++;
            }

            // Emit the entries as a stream. `sent` counts what was actually read so the
            // last packet always carries FLAG_STOP even if a late entry read failed.
            for (uint8_t index = 0; index < sent; index++) {
                FileEntry entry;
                if (!Storage.ReadFileEntry(index, &entry))
                    break;
                uint8_t flags = FLAG_TYPE;
                if (index == 0) flags |= FLAG_START;
                if (index == sent - 1) flags |= FLAG_STOP;

                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 flags, (const uint8_t *)&entry, sizeof(FileEntry));
                reply.frag_id = NextFragmentId(reply.flags);
                DispatchPacket(reply);
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
            if (frame.payload_len >= 12) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t size = *reinterpret_cast<const uint32_t *>(frame.payload + 8);

                bool ok = Storage.CreateFile(name, size);
                uint8_t status = ok ? 0x01 : 0x00;
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, &status, 1);
                DispatchPacket(reply);
            }
            break;
        }

        case 3: { // Delete File (Request: Name (8 bytes))
            if (frame.payload_len >= 8) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                Storage.DeleteFile(name);

                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(reply);
            }
            break;
        }

        case 4: { // Resize File (Request: Name (8 bytes) + New Size (4 bytes))
            if (frame.payload_len >= 12) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t new_size = *reinterpret_cast<const uint32_t *>(frame.payload + 8);

                bool ok = Storage.ResizeFile(name, new_size);
                uint8_t status = ok ? 0x01 : 0x00;
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, &status, 1);
                DispatchPacket(reply);
            }
            break;
        }

        case 5: { // Read File (Request: Name (8 bytes) + Offset start (4 bytes) + Number of bytes (4 bytes))
            if (frame.payload_len >= 16) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t offset = *reinterpret_cast<const uint32_t *>(frame.payload + 8);
                uint32_t num_bytes = *reinterpret_cast<const uint32_t *>(frame.payload + 12);

                uint32_t file_offset, file_size;
                if (Storage.GetFileInfo(name, &file_offset, &file_size) && offset < file_size) {
                    uint32_t remaining = file_size - offset;
                    if (num_bytes > remaining) num_bytes = remaining;

                    uint32_t read_ptr = file_offset + offset;

                    // Stream response back in chunks. A zero-length read still answers
                    // with an empty START|STOP frame so the client never hangs.
                    uint32_t sent_bytes = 0;
                    do {
                        if (num_bytes == 0) {
                            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                             FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                            DispatchPacket(reply);
                            break;
                        }

                        uint32_t chunk = num_bytes - sent_bytes;
                        if (chunk > MAX_PAYLOAD_SIZE - 1) chunk = MAX_PAYLOAD_SIZE - 1;

                        uint8_t temp_buf[MAX_PAYLOAD_SIZE];
                        Storage_FlashRead(read_ptr + sent_bytes, temp_buf, chunk);

                        uint8_t flags = FLAG_TYPE;
                        if (sent_bytes == 0) flags |= FLAG_START;
                        if (sent_bytes + chunk == num_bytes) flags |= FLAG_STOP;

                        PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                     flags, temp_buf, chunk);
                        reply.frag_id = NextFragmentId(reply.flags);
                        DispatchPacket(reply);

                        sent_bytes += chunk;
                    } while (sent_bytes < num_bytes);
                } else {
                    // File not found or invalid offset
                    PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                     FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                    DispatchPacket(reply);
                }
            }
            break;
        }

        case 6: { // Write Stream Open (Request: Name (8 bytes) + Offset start (4 bytes))
            if (frame.payload_len >= 12) {
                const char *name = reinterpret_cast<const char *>(frame.payload);
                uint32_t offset = *reinterpret_cast<const uint32_t *>(frame.payload + 8);

                // Resolve and cache the file location once, at stream open.
                uint32_t file_offset = 0, file_size = 0;
                bool file_ok = Storage.GetFileInfo(name, &file_offset, &file_size);
                if (file_ok && offset > file_size) file_ok = false; // start beyond EOF

                uint8_t cid_assigned = 0;
                if (file_ok) {
                    for (int stream_idx = 0; stream_idx < MAX_STREAMS; stream_idx++) {
                        if (!Storage.streams[stream_idx].active) {
                            Storage.streams[stream_idx].active = true;
                            Storage.streams[stream_idx].cid = 64 + stream_idx;
                            memcpy(Storage.streams[stream_idx].name, name, 8);
                            Storage.streams[stream_idx].current_offset = offset;
                            Storage.streams[stream_idx].file_offset = file_offset;
                            Storage.streams[stream_idx].file_size = file_size;
                            cid_assigned = Storage.streams[stream_idx].cid;
                            break;
                        }
                    }
                }

                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, &cid_assigned, 1);
                DispatchPacket(reply);
            }
            break;
        }

        case 7: { // Write Stream Close (Request: CID stream (1 byte))
            if (frame.payload_len >= 1) {
                uint8_t cid_close = frame.payload[0];
                for (int stream_idx = 0; stream_idx < MAX_STREAMS; stream_idx++) {
                    if (Storage.streams[stream_idx].active && Storage.streams[stream_idx].cid == cid_close) {
                        Storage.streams[stream_idx].active = false;
                        break;
                    }
                }
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(reply);
            }
            break;
        }

        default:
            break;
    }
}

