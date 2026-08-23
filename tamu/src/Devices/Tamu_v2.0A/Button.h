#pragma once

// Turns the notification LED on/off and updates the LED/button state in the block.
bool OnLEDStateChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len) {
    bool new_state = *static_cast<const bool*>(data);
    ESP_LOGI("HW_CONTROL", "Toggling LED to: %s", new_state ? "ON" : "OFF");

    // Access the structure using a safe static_cast
    auto* led_data = static_cast<LEDButtonStruct*>(block.Data);

    // Perform hardware action
    if (new_state) {
        PinModeOutput(LED_NOTIFICATION_PIN);
        led_data->ButtonState = false;
        PinLow(LED_NOTIFICATION_PIN);
    } else {
        PinHigh(LED_NOTIFICATION_PIN);
        // Pull-down input: the shared LED/button line floats low when idle, and a
        // button press pulls it high (ButtonUpdate reads active-high inverted).
        PinModeInputPullDown(LED_NOTIFICATION_PIN);
    }

    led_data->LEDState = new_state;
    return true;
}

// Samples the notification pin and updates the button state while the LED is off.
// Polarity per the hardware: pull-down input floats LOW when idle (ButtonState = false)
// and a button press pulls the line HIGH (ButtonState = true).
// Per Docs/Modules/Generic system blocks.md, pushing the button triggers the LED: a
// rising edge while the LED is off lights it. (The shared line cannot be read while the
// LED is driven, so "press again to turn off" is not detectable - turn it off remotely
// or via the LED write field.)
void ButtonUpdate()
{
    static bool prev_pressed = false;

    if (LedButton.LEDState != false)
    {
        prev_pressed = false;
        return;
    }

    bool pressed = PinRead(LED_NOTIFICATION_PIN);
    LedButton.ButtonState = pressed;

    if (pressed && !prev_pressed)
    {
        bool led_on = true;
        OnLEDStateChange(static_block_registry[0], 0, &led_on, sizeof(led_on));
    }
    prev_pressed = pressed;
}
