#pragma once

// Shared host-side bootloader plumbing (Docs/Services/Bootloader.md).
//
// Both bootloaders serve the exact same raw frame format: the core over its USB
// Serial/JTAG port into `ota_0`, the DAS over RS485 into its flash. Only the byte
// transport and the backing flash differ, so the read-response encoding lives here and
// each host supplies small callbacks for its transport. Frame reception and dispatch are
// NOT here: each host owns its transport's frame state machine (Devices/*/Bootloader.cpp),
// which is why the shared `ReceiveFrame`/`HostLoop` pair was removed - nothing called it
// (Cleanup pass 3, t_dbfb5bec).
//
// The helpers are header-only and templated on those callbacks so, at -Os/-flto, the
// host's transport inlines into the helper rather than costing an indirect call. The
// DAS image is 4 B under its 2 KB region, so keep the indirection count low.

#include <cstdint>
#include <cstring>

#include "Core/Functions/Bootloader.h"

namespace BootloaderHost
{
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
} // namespace BootloaderHost
