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
    inline int ReceiveFrame(uint8_t *Out, size_t Cap, BeginFrame Begin, ReadByte Read)
    {
        Begin();
        uint8_t b;
        for (;;)
        {
            if (!Read(&b)) return 0;
            if (b == Bootloader::START) break; // skip anything that is not a frame start
        }

        size_t Got = 0;
        Out[Got++] = b;
        if (!Read(&b)) return 0;
        Out[Got++] = b;

        uint16_t Need = Bootloader::FrameSize((uint8_t)(b & 0x07u));
        if (Need == 0 || Need > Cap) return 0;
        while (Got < Need)
        {
            if (!Read(&b)) return 0;
            Out[Got++] = b;
        }
        return Bootloader::Decode(Out, Need) != Bootloader::INVALID ? (int)Need : 0;
    }

    // Answers one read-request: reads `PAYLOAD_SIZE` bytes at `offset`, encodes the
    // response and sends it. `read` is bool(uint32_t, uint8_t *): true when the bytes
    // were read, false when `offset` is out of range (the buffer is then 0xFF/erased).
    // `send` is void(const uint8_t *, size_t).
    template <typename ReadPayload, typename Send>
    inline void HandleRead(uint32_t Offset, ReadPayload Read, Send SendCb)
    {
        uint8_t Data[Bootloader::PAYLOAD_SIZE];
        if (!Read(Offset, Data))
            memset(Data, 0xFF, sizeof(Data));

        uint8_t Resp[Bootloader::DATA_SIZE];
        Bootloader::EncodeReadResponse(Offset, Data, Resp);
        SendCb(Resp, sizeof(Resp));
    }

    // Serves raw frames forever. `OnWrite` is void(uint32_t, const uint8_t *), `OnRead` is
    // void(uint32_t) and `OnIdentify` is void(); the host's own handlers do the flashing
    // and send the identify response (its app size/name are build-time constants).
    template <typename BeginFrame, typename ReadByte, typename OnWrite, typename OnRead, typename OnIdentify>
    inline void HostLoop(BeginFrame Begin, ReadByte Read, OnWrite OnWriteCb, OnRead OnReadCb, OnIdentify OnIdentifyCb)
    {
        for (;;)
        {
            uint8_t Frame[Bootloader::MAX_FRAME_SIZE];
            int n = ReceiveFrame(Frame, sizeof(Frame), Begin, Read);
            if (n <= 0) continue;

            uint8_t Cmd = (uint8_t)(Frame[1] & 0x07u);
            uint32_t Offset = Bootloader::Offset(Frame);
            if (Cmd == Bootloader::CMD_IDENTIFY_REQ)
                OnIdentifyCb();
            else if (Cmd == Bootloader::CMD_WRITE)
                OnWriteCb(Offset, Bootloader::Payload(Frame));
            else if (Cmd == Bootloader::CMD_READ_REQ)
                OnReadCb(Offset);
        }
    }
} // namespace BootloaderHost
