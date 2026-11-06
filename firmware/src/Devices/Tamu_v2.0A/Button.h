#pragma once

// Turns the notification LED on/off and updates the LED/button state in the block.
// LEDState is field 3 of the LED-Button schema (Docs/Modules and blocks/Buttons &
// LEDS.md).
bool OnLEDStateChange(const StaticBlockDescriptor& Block, uint16_t Index, const void* Data, uint16_t DataLen) {
    if (DataLen != sizeof(bool)) return false;
    bool NewState = *static_cast<const bool*>(Data);
    if (!AppInterface.Connected)
        ESP_LOGI("HW_CONTROL", "Toggling LED to: %s", NewState ? "ON" : "OFF");

    // Access the structure using a safe static_cast
    auto* LedData = static_cast<LEDButtonVolatile*>(Block.VolatileData);

    // Perform hardware action. The LED is active-LOW (lights when the pin is
    // driven low; the button's pull-up holds the line high = LED off).
    if (NewState) {
        PinModeOutput(LED_NOTIFICATION_PIN);
        LedData->ButtonState = false;
        PinLow(LED_NOTIFICATION_PIN);
    } else {
        // Input with pull-up (active-low button: the line idles HIGH = LED off and
        // a press pulls it LOW). The internal pull-up guarantees a defined idle level
        // even if the board pull-up is absent.
        PinModeInputPullUp(LED_NOTIFICATION_PIN);
    }

    LedData->LEDState = NewState;
    return true;
}

// The GPIO8 BLE reset's sequencer (Blocks/BleResetHold.h) owns this pin while it runs: it drives the
// LED from the 2 s mark of a hold and through the five blinks after a clear. Declared here because
// this header comes first in Main.h's include order; defined in BleResetPin.h.
bool BleResetOwnsPin();

// Samples the notification pin and reports the button state while the LED is off.
// Polarity per the hardware: pulling the pin LOW lights the LED and reads as the
// button pressed (the line idles HIGH via the pull-up, so ButtonState = false at rest).
// The button only reports its state (field 0) - it does not drive the LED. While the
// LED is on the shared line is driven, so the button cannot be read (ButtonState = false).
//
// The GPIO2 line is also driven by the GPIO8 reset sequencer while it owns it (a recognised hold, or
// the blinks after a clear), and a line driven low reads as a pressed button - so the report is
// SUPPRESSED for exactly that long. The pin read itself is untouched: a press of the real button sets
// field 0 whenever the sequencer is idle, and a contact on the GPIO8 reset pad is not a button press
// (that is a different pad, and this read is GPIO2).
void ButtonUpdate()
{
    if (StaticVol.LedButton.LEDState != false || BleResetOwnsPin())
    {
        StaticVol.LedButton.ButtonState = false; // line is driven (LED) or the reset sequence owns it
        return;
    }

    StaticVol.LedButton.ButtonState = !PinRead(LED_NOTIFICATION_PIN);
}
