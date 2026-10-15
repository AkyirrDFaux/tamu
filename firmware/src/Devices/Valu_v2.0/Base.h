#pragma once

#include "ch32v20x.h"

// Shared platform primitives (Now/TimeFromBoot/Sleep/SleepMicro/GetFreeRAM, the SPL GPIO
// helpers and VOLTAGE) are identical on the DAS and the Valu - they live in
// Core/Devices/Base.h. Everything that differs per board (the pin modes this board uses
// and its RAM size) stays here.
#include "Core/Devices/Base.h"

// Returns the total available RAM in bytes.
uint32_t GetTotalRAM()
{
    // CH32V203G8 has 20 KB SRAM (board upload.maximum_ram_size).
    return 0x5000; // 20 KB = 20480 bytes
}

// Configures the given GPIO pin as a floating input.
void PinModeInput(GPIO_TypeDef *Port, uint16_t Pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = Pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(Port, &GPIO_InitStructure);
}

// Configures the given GPIO pin as an input with internal pull-down (the Valu's buttons are
// pull-down, active-high: the line idles LOW and a press drives it HIGH).
void PinModeInputPullDown(GPIO_TypeDef *Port, uint16_t Pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = Pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD; // Input Pull-Down
    GPIO_Init(Port, &GPIO_InitStructure);
}

