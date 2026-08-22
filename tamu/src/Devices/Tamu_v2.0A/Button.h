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
        PinModeInput(LED_NOTIFICATION_PIN);
    }

    led_data->LEDState = new_state;
    return true;
}

// Samples the notification pin and updates the button state while the LED is off.
void ButtonUpdate()
{
    if (LedButton.LEDState == false)
            LedButton.ButtonState = !PinRead(LED_NOTIFICATION_PIN);
}
