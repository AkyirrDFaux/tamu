#pragma once

#define LEDW GPIOD, GPIO_Pin_0
#define LEDR GPIOA, GPIO_Pin_1

// Latched bus error flag: set on any RS485 transmit failure, cleared on the next
// successful transmit. The main loop blinks the red LED while it is set (error
// indication has priority over the LED block value).
static bool DasErrorFlag = false;

// Shared platform primitives (Now/TimeFromBoot/Sleep/SleepMicro/GetFreeRAM, the SPL GPIO
// helpers and VOLTAGE) are identical on the DAS and the Valu - they live in
// Core/Devices/Base.h. Everything that differs per board (the port/pin maps, DasErrorFlag
// and the RAM size) stays here.
#include "Core/Devices/Base.h"

// Returns the total available RAM in bytes.
uint32_t GetTotalRAM()
{
    // CH32V003 has 2KB SRAM
    return 0x800; // 2KB = 2048 bytes
}
