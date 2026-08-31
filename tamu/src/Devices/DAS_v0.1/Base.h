#pragma once

#define LEDW GPIOD, GPIO_Pin_0
#define LEDR GPIOA, GPIO_Pin_1

#define VOLTAGE (3.3)

static uint32_t ms_accum = 0;
static uint32_t last_cnt = 0;
static uint32_t ms_rem = 0; // fractional milliseconds (cycles) carried between calls

// Returns the RAW time since boot in milliseconds, tracking SysTick rollovers and
// unaffected by any time offset. Uses only 32-bit math so the 64-bit division helper
// (__udivdi3) is not pulled in.
// The SysTick counter runs freely up at HCLK (48 MHz) with CMP = 0, so a single call may
// only span a few hundred cycles. A plain "elapsed / 48000" would truncate that to zero
// every call and freeze ms_accum in tight loops (e.g. Sleep). A remainder accumulator keeps
// the fractional cycles so the millisecond count advances correctly however often we're polled.
uint32_t TimeFromBoot(void) {
    static bool s_inited = false;
    uint32_t current_cnt = SysTick->CNT;
    // Prime the baseline on the first call. The WCH SysTick counter is not guaranteed to
    // start from 0 on every reset (debugger halt/resume, bootloader re-entry, etc.), so
    // anchoring to the live value keeps the very first TimeFromBoot() from adding a bogus chunk.
    if (!s_inited)
    {
        last_cnt = current_cnt;
        s_inited = true;
    }
    // Unsigned subtraction yields the elapsed cycles since the last call, correctly
    // accounting for a single SysTick rollover.
    uint32_t elapsed = current_cnt - last_cnt;
    last_cnt = current_cnt;
    uint32_t divisor = (SystemCoreClock / 1000);
    uint32_t total = elapsed + ms_rem;
    ms_rem = total % divisor;
    ms_accum += total / divisor;
    return ms_accum;
}

// Returns the current SYNCHRONIZED time in milliseconds (raw timer + time offset pushed
// by the core via Device service CID 11). TimeOffsetMs is declared in Core/Functions/
// SysFunctions.h, which is always included before this header in the translation unit.
uint32_t Now(void) {
    return TimeFromBoot() + TimeOffsetMs;
}

// Non-blocking busy-wait delay for `ms` milliseconds based on the SysTick counter.
void Sleep(uint32_t ms) {
    uint32_t start = Now();
    while ((Now() - start) < ms) {
    }
}

// Busy-wait delay for `us` microseconds using raw SysTick cycle counting.
void SleepMicro(uint32_t us)
{
    // Capture start time from SysTick
    uint32_t start_cycles = SysTick->CNT;
    
    // Convert microseconds to cycles
    // (SystemCoreClock / 1000000) is cycles per microsecond
    uint32_t target_cycles = (us * (SystemCoreClock / 1000000));
    
    // Spin until elapsed
    while ((SysTick->CNT - start_cycles) < target_cycles)
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
    uint32_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    return (int32_t)(sp - (uint32_t)&_ebss);
}

// Configures the given GPIO pin as a push-pull output.
void PinModeOutput(GPIO_TypeDef* port, uint16_t pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(port, &GPIO_InitStructure);
}

// Configures the given GPIO pin as an input with internal pull-down.
void PinModeInputPullDown(GPIO_TypeDef* port, uint16_t pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD; // Input Pull-Down
    GPIO_Init(port, &GPIO_InitStructure);
}

// Reads the current digital level of the given GPIO pin.
bool PinRead(GPIO_TypeDef* port, uint16_t pin)
{
    return GPIO_ReadInputDataBit(port, pin) != Bit_RESET;
}

// Drives the given GPIO pin high.
void PinHigh(GPIO_TypeDef* port, uint16_t pin)
{
    GPIO_WriteBit(port, pin, Bit_SET);
}

// Drives the given GPIO pin low.
void PinLow(GPIO_TypeDef* port, uint16_t pin)
{
    GPIO_WriteBit(port, pin, Bit_RESET);
}