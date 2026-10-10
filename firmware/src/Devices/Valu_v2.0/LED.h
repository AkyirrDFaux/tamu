#pragma once

// Valu LED-Button block (Docs/Devices.md: "LED-Button PA2 - Red, Active high"). The LED is
// driven by the LEDState field (field 3 of the LED-Button schema); the shared pin doubles as
// the button while the LED is off (the block's Button field, field 0). Active high: driving
// PA2 high lights the LED; the line idles low (pull-down) and a press drives it high.

#define VALU_LED_PORT GPIOA
#define VALU_LED_PIN  GPIO_Pin_2

// Turns the notification LED on/off. LEDState is field 3 of the LED-Button schema
// (Docs/Modules and blocks/Buttons & LEDS.md).
bool OnLEDStateChange(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t DataLen)
{
    if (DataLen != sizeof(bool)) return false;
    bool NewState = *static_cast<const bool *>(Data);

    auto *LedData = static_cast<LEDButtonVolatile *>(Block.VolatileData);

    if (NewState)
    {
        // Drive the pin high (active high LED) and mark the shared line unreadable.
        PinModeOutput(VALU_LED_PORT, VALU_LED_PIN);
        LedData->ButtonState = false;
        PinHigh(VALU_LED_PORT, VALU_LED_PIN);
    }
    else
    {
        // Off: release the line to an input with pull-down, so the button can be read.
        PinModeInputPullDown(VALU_LED_PORT, VALU_LED_PIN);
    }

    LedData->LEDState = NewState;
    return true;
}

// Samples the shared LED/button pin and reports the button state while the LED is off
// (active-high: a press drives the line high). While the LED is on the line is driven, so
// the button cannot be read. Called every main-loop iteration.
void LEDButtonUpdate()
{
    if (staticVol.ledButton.LEDState != false)
    {
        staticVol.ledButton.ButtonState = false; // line is driven by the LED: not readable
        return;
    }
    staticVol.ledButton.ButtonState = PinRead(VALU_LED_PORT, VALU_LED_PIN);
}
