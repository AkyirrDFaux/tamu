#pragma once

#include <cstdint>

// CH32V003 reset-cause flags, as reported once per boot through the Log service
// (Docs/Services/Log Handler.md: a node reports outbound to the local core, 0.1).
//
// The six flags sit in RCC->RSTSCKR bits 26..31 and are contiguous, so the whole cause is one
// byte: bit 0 is RSTSCKR bit 26, bit 5 is bit 31. That is a shift+mask instead of six
// branches, and the device header (Main.h) static_asserts every constant below against the
// SPL's RCC_*RSTF macros - a header drift then breaks the build instead of silently
// renumbering the report.
//
// Host-clean on purpose (no chip header, no globals): test/native/reset_test.cpp pins the map
// so the byte the core receives cannot rot unnoticed.
//
// What each flag means for THIS node - the DAS image arms no watchdog, so a set IWDG/WWDG bit
// means either an external agent or a stale flag, never this firmware:
//   PIN    reset pin / button-driven reset (the bootloader is entered by holding the button)
//   POR    the supply dropped below the power-on threshold - the "power" answer
//   SW     the firmware reset itself (NVIC_SystemReset or a fault handler)
//   IWDG   independent watchdog reset
//   WWDG   window watchdog reset
//   LPWR   low-power reset (the SPL's name for it; STM32F1 calls the same bit LPWR, and the
//          card called it LOCKUP - on the CH32V003 the header says LPWRRSTF)
#define DAS_RST_PIN  0x04000000u // RSTSCKR bit 26
#define DAS_RST_POR  0x08000000u // RSTSCKR bit 27
#define DAS_RST_SW   0x10000000u // RSTSCKR bit 28
#define DAS_RST_IWDG 0x20000000u // RSTSCKR bit 29
#define DAS_RST_WWDG 0x40000000u // RSTSCKR bit 30
#define DAS_RST_LPWR 0x80000000u // RSTSCKR bit 31

// The six flags as one byte, in register bit order (bit 0 = RSTSCKR bit 26). Bits outside
// 26..31 - RMVF, LSION/LSIRDY - are masked off, so the byte is the cause and nothing else.
constexpr uint8_t DasResetCauseFlags(uint32_t RstSckr)
{
    return (uint8_t)((RstSckr >> 26) & 0x3Fu);
}
