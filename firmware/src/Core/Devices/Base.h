#pragma once

// Cross-device platform primitives shared by the DAS v0.1 (CH32V003) and the Valu v2.0
// (CH32V203G8R6): the same CH32 SPL GPIO/SysTick idioms, verified byte-for-byte on both.
// The includer must have pulled in its own chip header (ch32v00x.h / ch32v20x.h) first,
// exactly as the per-device Base.h did before. Everything that actually differs per board
// stays in the per-device Base.h: the port/pin maps, GetTotalRAM (different SRAM sizes) and
// any pin mode only one board uses (e.g. the Valu's floating/pull-down inputs).

// Supply voltage. Identical on the DAS and the Valu (different pin maps, same 3.3 V supply).
// Tamu_v2.0A/Base.h still carries its own copy until that device joins the shared family.
#define VOLTAGE (3.3)

static uint32_t MsAccum = 0;
static uint32_t LastCnt = 0;
static uint32_t MsRem = 0; // fractional milliseconds (cycles) carried between calls

// Returns the RAW time since boot in milliseconds, tracking SysTick rollovers and
// unaffected by any time offset. Uses only 32-bit math so the 64-bit division helper
// (__udivdi3) is not pulled in.
// The SysTick counter runs freely up at HCLK with CMP = 0, so a single call may only span a few
// hundred cycles. A plain "elapsed / (HCLK/1000)" would truncate that to zero every call and
// freeze MsAccum in tight loops (e.g. Sleep). A remainder accumulator keeps the fractional
// cycles so the millisecond count advances correctly however often we're polled.
uint32_t TimeFromBoot(void) {
    static bool Inited = false;
    uint32_t CurrentCnt = SysTick->CNT;
    // Prime the baseline on the first call. The WCH SysTick counter is not guaranteed to
    // start from 0 on every reset (debugger halt/resume, bootloader re-entry, etc.), so
    // anchoring to the live value keeps the very first TimeFromBoot() from adding a bogus chunk.
    if (!Inited)
    {
        LastCnt = CurrentCnt;
        Inited = true;
    }
    // Unsigned subtraction yields the elapsed cycles since the last call, correctly
    // accounting for a single SysTick rollover.
    uint32_t Elapsed = CurrentCnt - LastCnt;
    LastCnt = CurrentCnt;
    uint32_t Divisor = (SystemCoreClock / 1000);
    uint32_t Total = Elapsed + MsRem;
    MsRem = Total % Divisor;
    MsAccum += Total / Divisor;
    return MsAccum;
}

// Returns the current SYNCHRONIZED time in milliseconds (raw timer + the offset this node
// computed from its own Device service CID 3 (TimeSync) exchange with the core, including
// the drift extrapolation between syncs). ApplyTimeOffset is in Core/Functions/
// SysFunctions.h, which is always included before this header in the translation unit.
uint32_t Now(void) {
    return ApplyTimeOffset(TimeFromBoot());
}

// Non-blocking busy-wait delay for `ms` milliseconds based on the SysTick counter.
void Sleep(uint32_t Ms) {
    uint32_t Start = Now();
    while ((Now() - Start) < Ms) {
    }
}

// Busy-wait delay for `us` microseconds using raw SysTick cycle counting.
void SleepMicro(uint32_t Us)
{
    // Capture start time from SysTick
    uint32_t StartCycles = SysTick->CNT;

    // Convert microseconds to cycles
    // (SystemCoreClock / 1000000) is cycles per microsecond
    uint32_t TargetCycles = (Us * (SystemCoreClock / 1000000));

    // Spin until elapsed
    while ((SysTick->CNT - StartCycles) < TargetCycles)
    {
        // Compiler barrier to prevent optimization
        __asm__ volatile ("nop");
    }
}

// Returns the approximate free RAM (bytes) between the end of BSS and the current
// stack pointer (read live from SP, not the boot-time linker symbol).
int32_t GetFreeRAM()
{
    extern uint32_t _ebss;
    uint32_t Sp;
    __asm__ volatile("mv %0, sp" : "=r"(Sp));
    return (int32_t)(Sp - (uint32_t)&_ebss);
}

// Configures the given GPIO pin as a push-pull output.
void PinModeOutput(GPIO_TypeDef* Port, uint16_t Pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = Pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(Port, &GPIO_InitStructure);
}

// Configures the given GPIO pin as an input with internal pull-up (active-low buttons).
void PinModeInputPullUp(GPIO_TypeDef* Port, uint16_t Pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = Pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU; // Input Pull-Up
    GPIO_Init(Port, &GPIO_InitStructure);
}

// Reads the current digital level of the given GPIO pin.
bool PinRead(GPIO_TypeDef* Port, uint16_t Pin)
{
    return GPIO_ReadInputDataBit(Port, Pin) != Bit_RESET;
}

// Drives the given GPIO pin high.
void PinHigh(GPIO_TypeDef* Port, uint16_t Pin)
{
    GPIO_WriteBit(Port, Pin, Bit_SET);
}

// Drives the given GPIO pin low.
void PinLow(GPIO_TypeDef* Port, uint16_t Pin)
{
    GPIO_WriteBit(Port, Pin, Bit_RESET);
}
