#pragma once

// Bootloader pieces that are byte-identical between the DAS v0.1 (CH32V003) and the Valu
// v2.0 (CH32V203) bootloaders: the direct-register GPIO pin config used before the SPL/clock
// setup (below), plus the build-time identify-response frame builder - which the ESP32-C3
// core bootloader shares too, so it lives in BootloaderIdentify.h and is included, not
// copied. Everything else stays in each Bootloader.cpp - the targets differ in flash APIs,
// transport (RS485 UART vs USB CDC) and address/geometry constants.
//
// The includer must have pulled in its chip header (ch32v00x.h / ch32v20x.h).

#include "Core/Devices/BootloaderIdentify.h"

// GPIO direct-register config: CFGLR is 4 bits per pin (valid for pins < 8, which is every
// pin both bootloaders touch).
static void GpioCfg(GPIO_TypeDef *Port, uint32_t Pin, uint32_t Cfg)
{
    uint32_t Shift = Pin * 4u;
    Port->CFGLR = (Port->CFGLR & ~(0xFu << Shift)) | (Cfg << Shift);
}

static void GpioOutPP(GPIO_TypeDef *Port, uint32_t Pin) { GpioCfg(Port, Pin, 0x3u); } // out push-pull 50 MHz
