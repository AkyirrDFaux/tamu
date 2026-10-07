#pragma once

#include "ch32v20x.h"

// Platform primitives for the Valu v2.0 (CH32V203G8R6). Mirrors Devices/DAS_v0.1/Base.h -
// the same CH32 SPL GPIO/SysTick idioms, adapted to the CH32V203 (144 MHz clock, 20 KB RAM,
// GPIO ports A/B). The core (Core/Functions/SysFunctions.h) expects exactly this interface:
// Now/TimeFromBoot/Sleep/SleepMicro/GetFreeRAM and the pin primitives. GetTotalRAM is used
// by the System block's RAM fields.

// NOTE: duplicates Tamu_v2.0A/Base.h's VOLTAGE; kept per-device until a shared Base is
// factored out (same reason as the DAS: the targets have different pin maps but the same
// supply voltage).
#define VOLTAGE (3.3)

static uint32_t ms_accum = 0;
static uint32_t last_cnt = 0;
static uint32_t ms_rem = 0; // fractional milliseconds (cycles) carried between calls

// Returns the RAW time since boot in milliseconds, tracking SysTick rollovers and unaffected
// by any time offset. Runs at HCLK (144 MHz here); the SysTick free-runs with CMP = 0, so a
// single call spans only a few hundred cycles and the remainder accumulator (as on the DAS)
// keeps the millisecond count advancing however often we are polled. 32-bit math only, so the
// 64-bit division helper (__udivdi3) is not pulled in.
uint32_t TimeFromBoot(void)
{
    static bool s_inited = false;
    uint32_t current_cnt = SysTick->CNT;
    // Prime the baseline on the first call (the WCH SysTick counter is not guaranteed to
    // start at 0 on every reset).
    if (!s_inited)
    {
        last_cnt = current_cnt;
        s_inited = true;
    }
    uint32_t elapsed = current_cnt - last_cnt;
    last_cnt = current_cnt;
    uint32_t divisor = (SystemCoreClock / 1000);
    uint32_t total = elapsed + ms_rem;
    ms_rem = total % divisor;
    ms_accum += total / divisor;
    return ms_accum;
}

// Returns the current SYNCHRONIZED time in milliseconds (raw timer + the offset this device
// computed from a Device service CID 3 TimeSync exchange). ApplyTimeOffset lives in
// Core/Functions/SysFunctions.h, always included before this header in the translation unit.
uint32_t Now(void)
{
    return ApplyTimeOffset(TimeFromBoot());
}

// Busy-wait delay for `ms` milliseconds based on the SysTick counter.
void Sleep(uint32_t ms)
{
    uint32_t start = Now();
    while ((Now() - start) < ms)
    {
    }
}

// Busy-wait delay for `us` microseconds using raw SysTick cycle counting.
void SleepMicro(uint32_t us)
{
    uint32_t start_cycles = SysTick->CNT;
    uint32_t target_cycles = (us * (SystemCoreClock / 1000000));
    while ((SysTick->CNT - start_cycles) < target_cycles)
    {
        __asm__ volatile("nop");
    }
}

// Returns the approximate free RAM (bytes) between the end of BSS and the current stack
// pointer (read live from SP, not the boot-time linker symbol).
int32_t GetFreeRAM()
{
    extern uint32_t _ebss;
    uint32_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    return (int32_t)(sp - (uint32_t)&_ebss);
}

// Returns the total available RAM in bytes.
uint32_t GetTotalRAM()
{
    // CH32V203G8 has 20 KB SRAM (board upload.maximum_ram_size).
    return 0x5000; // 20 KB = 20480 bytes
}

// Configures the given GPIO pin as a push-pull output. `cfg` 0x3 in CFGLR/CFGHR = output
// push-pull, 50 MHz (the same 4-bit encoding the Valu bootloader drives PA2 with).
void PinModeOutput(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(port, &GPIO_InitStructure);
}

// Configures the given GPIO pin as a floating input.
void PinModeInput(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(port, &GPIO_InitStructure);
}

// Configures the given GPIO pin as an input with internal pull-down (the Valu's buttons are
// pull-down, active-high: the line idles LOW and a press drives it HIGH).
void PinModeInputPullDown(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD; // Input Pull-Down
    GPIO_Init(port, &GPIO_InitStructure);
}

// Configures the given GPIO pin as an input with internal pull-up.
void PinModeInputPullUp(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU; // Input Pull-Up
    GPIO_Init(port, &GPIO_InitStructure);
}

// Reads the current digital level of the given GPIO pin.
bool PinRead(GPIO_TypeDef *port, uint16_t pin)
{
    return GPIO_ReadInputDataBit(port, pin) != Bit_RESET;
}

// Drives the given GPIO pin high.
void PinHigh(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_WriteBit(port, pin, Bit_SET);
}

// Drives the given GPIO pin low.
void PinLow(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_WriteBit(port, pin, Bit_RESET);
}
