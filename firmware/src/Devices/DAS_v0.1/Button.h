#pragma once

// DAS Button block (Docs/Devices.md: "Button PC0 - Requires pullup, active low").
// The line idles HIGH via the pull-up; a press pulls it LOW. The block's edge
// detection/counter fields follow Docs/Modules and blocks/Buttons & LEDS.md.
#define DAS_BUTTON_PORT GPIOC
#define DAS_BUTTON_PIN  GPIO_Pin_0

void DasButtonInit()
{
    PinModeInputPullUp(DAS_BUTTON_PORT, DAS_BUTTON_PIN);
}

// Samples the button pin and updates the Button block state. Called every main-loop
// iteration.
void DasButtonUpdate()
{
    DasButton.ButtonState = !PinRead(DAS_BUTTON_PORT, DAS_BUTTON_PIN);
}