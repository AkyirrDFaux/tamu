#pragma once

#include <cstdint>

#include "Core/Functions/Bootloader.h"

// The build-time identify-response frame. Byte-identical on all three bootloaders (ESP32-C3
// core, CH32V003 DAS, CH32V203 Valu): the response is entirely build-time constant (the app
// space size and the device name), so the whole frame - control/parity included - is
// precomputed and lives in flash. Nothing is built at runtime.
//
// It lives here rather than in Core/Devices/BootloaderCommon.h because that header also
// carries the CH32 direct-register GPIO config (GPIO_TypeDef/CFGLR), which the core's ESP32
// bootloader has no way to compile. This header needs nothing but Core/Functions/Bootloader.h.

struct IdentifyFrame
{
    uint8_t Bytes[Bootloader::DATA_SIZE];
};

constexpr IdentifyFrame MakeIdentify(uint32_t Space, const char *Name)
{
    IdentifyFrame Frame{};
    Frame.Bytes[0] = Bootloader::START;
    Frame.Bytes[1] = Bootloader::CMD_IDENTIFY_RESP;
    Frame.Bytes[2] = (uint8_t)(Space & 0xFFu);
    Frame.Bytes[3] = (uint8_t)((Space >> 8) & 0xFFu);
    Frame.Bytes[4] = (uint8_t)((Space >> 16) & 0xFFu);
    Frame.Bytes[5] = (uint8_t)((Space >> 24) & 0xFFu);
    for (uint8_t i = 0; i < Bootloader::NAME_SIZE; i++)
    {
        if (Name[i] == '\0')
            break;
        Frame.Bytes[6 + i] = (uint8_t)Name[i];
    }
    Frame.Bytes[Bootloader::DATA_SIZE - 1] = Bootloader::END;
    Frame.Bytes[1] |= (uint8_t)(Bootloader::Parity(Frame.Bytes, Bootloader::DATA_SIZE) << 3);
    return Frame;
}
