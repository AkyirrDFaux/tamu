#pragma once

#include "Core/Devices/ButtonSampling.h"

// DAS Button block (Docs/Devices.md: "Button PC0 - Requires pullup, active low").
// The line idles HIGH via the pull-up; a press pulls it LOW. The block's edge
// detection/counter fields follow Docs/Modules and blocks/Buttons & LEDS.md.
#define DAS_BUTTON_PORT GPIOC
#define DAS_BUTTON_PIN  GPIO_Pin_0
#define DAS_BUTTON_EXTI EXTI_Line0

// Arms the press capture: EXTI line 0 on PC0, falling edge only.
//
// This is deliberately NOT an interrupt. INTENR stays 0 and the PFIC line is never enabled, so
// no ISR, no vector slot and no handler prologue is paid for - what the loop needs is not a
// callback, it is the LATCH: the pending bit in EXTI->INTFR is set by the edge detector and
// stays set until software clears it, whatever the main loop is doing at the time. Polling that
// bit is the edge-triggered capture (ButtonSampling.h has the rule), and it is the only shape
// that fits the DAS's remaining code space.
void DasButtonInit()
{
    PinModeInputPullUp(DAS_BUTTON_PORT, DAS_BUTTON_PIN);
    // AFIO->EXTICR picks which port drives EXTI line 0, and AFIO is clocked through APB2. Written
    // as registers rather than through the SPL: the DAS has ~100 B of code space left, and the
    // SPL's EXTI_Init/GPIO_EXTILineConfig pull in a struct fill and two more calls for two stores.
    RCC->APB2PCENR |= RCC_APB2Periph_AFIO;
    AFIO->EXTICR = (AFIO->EXTICR & ~(uint32_t)AFIO_EXTICR1_EXTI0) | AFIO_EXTICR1_EXTI0_PC; // line 0 <- PC0
    EXTI->FTENR |= DAS_BUTTON_EXTI;
    // Drop whatever the arming itself latched (the pin is sampled before the edge filter
    // settles), so a real press is never reported twice by the first pass.
    EXTI->INTFR = DAS_BUTTON_EXTI;
}

// Samples the button pin and updates the Button block state. Called every main-loop
// iteration: the reported level is a captured press OR the line low now, and the capture is
// consumed straight after it is read (one tap = one press however short it was, see
// ButtonSampling.h).
void DasButtonUpdate()
{
    StaticVol.Button.ButtonState =
        ButtonSample(EXTI->INTFR & DAS_BUTTON_EXTI, !PinRead(DAS_BUTTON_PORT, DAS_BUTTON_PIN));
    EXTI->INTFR = DAS_BUTTON_EXTI;
}
