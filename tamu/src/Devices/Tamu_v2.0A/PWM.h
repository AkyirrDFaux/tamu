#include "driver/ledc.h"

// Configures LEDC timers and channels for both fans (25 kHz PWM on pins 6 and 10).
void SetupFanPWM() {
    // Fan 1 (Timer 0, Channel 0, Pin 6)
    ledc_channel_config_t chan1 = {
        .gpio_num = 6,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {0},
        .deconfigure = false,
    };
    ledc_channel_config(&chan1);

    ledc_timer_config_t timer1 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };

    ledc_timer_config(&timer1);

    // Fan 2 (Timer 1, Channel 1, Pin 10)
    ledc_channel_config_t chan2 = {
        .gpio_num = 10,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {0},
        .deconfigure = false,
    };
    ledc_channel_config(&chan2);

    ledc_timer_config_t timer2 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };

    ledc_timer_config(&timer2);
}

// Reconfigures the fan PWM frequency (stored in the block) on the timer matching the given block.
bool OnPWMFrequencyChange(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t data_len)
{
    uint32_t new_freq = *static_cast<const uint32_t *>(data);

    // Determine which timer to update based on the pointer
    ledc_timer_t timer = (block.Data == &Fan1) ? LEDC_TIMER_0 : LEDC_TIMER_1;

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = timer,
        .freq_hz = new_freq,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };

    if (ledc_timer_config(&ledc_timer) != ESP_OK) {
        return false;
    }

    // Update RAM state
    auto *fan = static_cast<PWMStruct *>(block.Data);
    fan->PWMFreq = new_freq;

    return true;
}

// Clamps the new duty (0-100%), converts it to the LEDC duty value and applies it to the fan's PWM channel.
bool OnPWMDutyChange(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t data_len)
{
    Number new_duty = *static_cast<const Number *>(data);

    // Clamp value to 0-100%
    if (new_duty > N(100))
        new_duty = N(100);
    else if (new_duty < N(0))
        new_duty = N(0);

    // Calculate duty cycle: (new_duty / 100 * 1023) >> 16
    uint32_t duty = (new_duty.Value / 100 * 1023) >> 16;

    // Map data_ptr to channel
    ledc_channel_t channel = (block.Data == &Fan1) ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);

    // Update RAM state
    auto *fan = static_cast<PWMStruct *>(block.Data);
    fan->Duty = new_duty;

    return (ledc_update_duty(LEDC_LOW_SPEED_MODE, channel) == ESP_OK);
}