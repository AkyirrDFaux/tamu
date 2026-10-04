#pragma once

// Shared host-side bootloader plumbing (Docs/Services/Bootloader.md).
//
// Both bootloaders serve the exact same raw frame format: the core over its USB
// Serial/JTAG port into `ota_0`, the DAS over RS485 into its flash. Only the byte
// transport and the backing flash differ, so the receive / read-response / dispatch
// logic lives here and each host supplies small callbacks for its transport.
//
// The helpers are header-only and templated on those callbacks so, at -Os/-flto, the
// host's transport inlines into the helper rather than costing an indirect call. The
// DAS image has only ~200 B of flash free, so keep the indirection count low.

#include <cstdint>
#include <cstring>

#include "Core/Functions/Bootloader.h"

namespace BootloaderHost
{
    // Reads one raw frame into `out`; returns its length or 0 on timeout/invalid.
    // `begin` resets the host's per-frame deadline; `read` is bool(uint8_t *): true
    // when one byte was received. Keeping the deadline reset in `begin` preserves a
    // whole-frame (not per-byte) timeout for hosts that use one.
    template <typename BeginFrame, typename ReadByte>
    inline int ReceiveFrame(uint8_t *out, size_t cap, BeginFrame begin, ReadByte read)
    {
        begin();
        uint8_t b;
        for (;;)
        {
            if (!read(&b)) return 0;
            if (b == Bootloader::START) break; // skip anything that is not a frame start
        }

        size_t got = 0;
        out[got++] = b;
        if (!read(&b)) return 0;
        out[got++] = b;

        uint16_t need = Bootloader::FrameSize((uint8_t)(b & 0x03u));
        if (need == 0 || need > cap) return 0;
        while (got < need)
        {
            if (!read(&b)) return 0;
            out[got++] = b;
        }
        return Bootloader::Decode(out, need) ? (int)need : 0;
    }

    // Answers one read-request: reads `PAYLOAD_SIZE` bytes at `offset`, encodes the
    // response and sends it. `read` is bool(uint32_t, uint8_t *): true when the bytes
    // were read, false when `offset` is out of range (the buffer is then 0xFF/erased).
    // `send` is void(const uint8_t *, size_t).
    template <typename ReadPayload, typename Send>
    inline void HandleRead(uint32_t offset, ReadPayload read, Send send)
    {
        uint8_t data[Bootloader::PAYLOAD_SIZE];
        if (!read(offset, data))
            memset(data, 0xFF, sizeof(data));

        uint8_t resp[Bootloader::DATA_SIZE];
        Bootloader::EncodeReadResponse(offset, data, resp);
        send(resp, sizeof(resp));
    }

    // Serves raw frames forever. `onWrite` is void(uint32_t, const uint8_t *) and
    // `onRead` is void(uint32_t); the host's own write/read handlers do the flashing.
    template <typename BeginFrame, typename ReadByte, typename OnWrite, typename OnRead>
    inline void HostLoop(BeginFrame begin, ReadByte read, OnWrite onWrite, OnRead onRead)
    {
        for (;;)
        {
            uint8_t frame[Bootloader::MAX_FRAME_SIZE];
            int n = ReceiveFrame(frame, sizeof(frame), begin, read);
            if (n <= 0) continue;

            uint8_t cmd = (uint8_t)(frame[1] & 0x03u);
            uint32_t offset = Bootloader::Offset(frame);
            if (cmd == Bootloader::CMD_WRITE)
                onWrite(offset, Bootloader::Payload(frame));
            else if (cmd == Bootloader::CMD_READ_REQ)
                onRead(offset);
        }
    }
} // namespace BootloaderHost
