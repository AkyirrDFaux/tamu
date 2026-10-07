#pragma once

// Valu buttons (Docs/Devices.md: "PB15, PB14, PB13 - Requires pulldown, active high"). Three
// independent Button blocks. The line idles LOW via the internal pull-down; a press drives it
// HIGH. The Button block's raw state is field 0 (Docs/Modules and blocks/Buttons & LEDS.md).

static GPIO_TypeDef *const s_button_port[3] = {GPIOB, GPIOB, GPIOB};
static const uint16_t s_button_pin[3] = {GPIO_Pin_15, GPIO_Pin_14, GPIO_Pin_13};

// Configures the three button pins as inputs with pull-down.
void ButtonsInit()
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    for (uint8_t i = 0; i < 3; i++)
        PinModeInputPullDown(s_button_port[i], s_button_pin[i]);
}

// Samples the three button pins into their Button block states. Called every main-loop
// iteration.
void ButtonsUpdate()
{
    for (uint8_t i = 0; i < 3; i++)
        staticVol.button[i].ButtonState = PinRead(s_button_port[i], s_button_pin[i]);
}
