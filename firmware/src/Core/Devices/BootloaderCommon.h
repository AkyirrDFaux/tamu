#pragma once

// Bootloader pieces that are byte-identical between the DAS v0.1 (CH32V003) and the Valu
// v2.0 (CH32V203) bootloaders: the direct-register GPIO pin config used before the SPL/clock
// setup, and the build-time identify-response frame builder. Everything else stays in each
// Bootloader.cpp - the two target different chips with different flash APIs, a different
// transport (RS485 UART vs USB CDC), and different address/geometry constants.
//
// The includer must have pulled in its chip header (ch32v00x.h / ch32v20x.h) and
// Core/Functions/Bootloader.h first.

// GPIO direct-register config: CFGLR is 4 bits per pin (valid for pins < 8, which is every
// pin both bootloaders touch).
static void GpioCfg(GPIO_TypeDef *Port, uint32_t Pin, uint32_t Cfg)
{
    uint32_t Shift = Pin * 4u;
    Port->CFGLR = (Port->CFGLR & ~(0xFu << Shift)) | (Cfg << Shift);
}

static void GpioOutPP(GPIO_TypeDef *Port, uint32_t Pin) { GpioCfg(Port, Pin, 0x3u); } // out push-pull 50 MHz

// The identify response is entirely build-time constant (app size + name), so the whole frame
// - control/parity included - is precomputed and lives in flash. Nothing is built at runtime.
struct IdentifyFrame { uint8_t Bytes[Bootloader::DATA_SIZE]; };

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
        if (Name[i] == '\0') break;
        Frame.Bytes[6 + i] = (uint8_t)Name[i];
    }
    Frame.Bytes[Bootloader::DATA_SIZE - 1] = Bootloader::END;
    Frame.Bytes[1] |= (uint8_t)(Bootloader::Parity(Frame.Bytes, Bootloader::DATA_SIZE) << 3);
    return Frame;
}
