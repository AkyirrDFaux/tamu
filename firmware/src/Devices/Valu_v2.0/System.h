#pragma once

// The Valu app is always launched by the Valu bootloader (Devices/Valu_v2.0/Bootloader.cpp), which
// has already brought SYSCLK up on the 144 MHz PLL. The framework's reset code calls SystemInit()
// a SECOND time; that routine clears PLLON and re-runs the whole SetSysClock sequence, which is a
// PLL disable/enable cycle on this part - not a state it is designed to restart from.
//
// Devices/Valu_v2.0/startup_ch32v20x_D6.S (this env's custom startup, board_build.startup) calls
// ValuClockInit instead of SystemInit directly, so the framework's clock setup only runs when the
// clock is NOT already up (e.g. a cold start with no bootloader in front of it).
extern "C" void SystemInit(void); // the framework's: System/ch32v20x/system_ch32v20x.c

extern "C" void ValuClockInit(void)
{
    if (RCC->CTLR & RCC_PLLON)
        return; // bootloader already enabled and configured the PLL - leave the clock alone
    SystemInit();
}
