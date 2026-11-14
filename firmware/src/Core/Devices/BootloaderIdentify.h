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
    uint8_t Bytes[Bootloader::IDENTIFY_RESP_SIZE];
};

// App size, then the flash page size (what an erase request's page is measured in), then the name.
constexpr IdentifyFrame MakeIdentify(uint32_t Space, uint16_t PageSize, const char *Name)
{
    IdentifyFrame Frame{};
    Frame.Bytes[0] = Bootloader::START;
    Frame.Bytes[1] = Bootloader::CMD_IDENTIFY_RESP;
    Frame.Bytes[2] = (uint8_t)(Space & 0xFFu);
    Frame.Bytes[3] = (uint8_t)((Space >> 8) & 0xFFu);
    Frame.Bytes[4] = (uint8_t)((Space >> 16) & 0xFFu);
    Frame.Bytes[5] = (uint8_t)((Space >> 24) & 0xFFu);
    Frame.Bytes[6] = (uint8_t)(PageSize & 0xFFu);
    Frame.Bytes[7] = (uint8_t)((PageSize >> 8) & 0xFFu);
    for (uint8_t i = 0; i < Bootloader::NAME_SIZE; i++)
    {
        if (Name[i] == '\0')
            break;
        Frame.Bytes[Bootloader::IDENTIFY_NAME_AT + i] = (uint8_t)Name[i];
    }
    Frame.Bytes[Bootloader::IDENTIFY_RESP_SIZE - 1] = Bootloader::END;
    Frame.Bytes[1] |= (uint8_t)(Bootloader::Parity(Frame.Bytes, Bootloader::IDENTIFY_RESP_SIZE) << 4);
    return Frame;
}
