#pragma once

// Turns the notification LED on/off and updates the LED/button state in the block.
bool OnLEDStateChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len) {
    bool new_state = *static_cast<const bool*>(data);
    ESP_LOGI("HW_CONTROL", "Toggling LED to: %s", new_state ? "ON" : "OFF");

    // Access the structure using a safe static_cast
    auto* led_data = static_cast<LEDButtonStruct*>(block.Data);

    // Perform hardware action. The LED is active-LOW (lights when the pin is
    // driven low; the button's pull-up holds the line high = LED off).
    if (new_state) {
        PinModeOutput(LED_NOTIFICATION_PIN);
        led_data->ButtonState = false;
        PinLow(LED_NOTIFICATION_PIN);
    } else {
        // Input with pull-up (active-low button: the line idles HIGH = LED off and
        // a press pulls it LOW). The internal pull-up guarantees a defined idle level
        // even if the board pull-up is absent.
        PinModeInputPullUp(LED_NOTIFICATION_PIN);
    }

    led_data->LEDState = new_state;
    return true;
}

// Samples the notification pin and reports the button state while the LED is off.
// Polarity per the hardware: pulling the pin LOW lights the LED and reads as the
// button pressed (the line idles HIGH via the pull-up, so ButtonState = false at rest).
// The button only reports its state (Out field) - it does not drive the LED. While the
// LED is on the shared line is driven, so the button cannot be read (ButtonState = false).
void ButtonUpdate()
{
    if (LedButton.LEDState != false)
    {
        LedButton.ButtonState = false; // line is driven by the LED: not readable
        return;
    }

    LedButton.ButtonState = !PinRead(LED_NOTIFICATION_PIN);
}
