#pragma once

#include "driver/ledc.h"

// Maps a fan block to its index (Fan1 -> 0, Fan2 -> 1), or -1 when the block is not one
// of the two fan instances. The LEDC timer/channel pairs follow the same index.
static int FanIndex(const StaticBlockDescriptor &Block)
{
    if (Block.PersistentData == &StaticPer.Fan[0]) return 0;
    if (Block.PersistentData == &StaticPer.Fan[1]) return 1;
    return -1;
}

// Configures LEDC timers and channels for both fans (25 kHz PWM on pins 6 and 10).
void SetupFanPWM() {
    // Fan 1 (Timer 0, Channel 0, Pin 6)
    ledc_channel_config_t Chan1 = {
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
    ledc_channel_config(&Chan1);

    ledc_timer_config_t Timer1 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };

    ledc_timer_config(&Timer1);

    // Fan 2 (Timer 1, Channel 1, Pin 10)
    ledc_channel_config_t Chan2 = {
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
    ledc_channel_config(&Chan2);

    ledc_timer_config_t Timer2 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };

    ledc_timer_config(&Timer2);
}

// Reconfigures the fan PWM frequency (stored in the block) on the timer matching the given block.
bool OnPWMFrequencyChange(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t DataLen)
{
    uint32_t NewFreq = *static_cast<const uint32_t *>(Data);

    // Determine which timer to update based on the block instance
    int FanSlot = FanIndex(Block);
    if (FanSlot < 0)
        return false;
    ledc_timer_t Timer = (FanSlot == 0) ? LEDC_TIMER_0 : LEDC_TIMER_1;

    ledc_timer_config_t LedcTimer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = Timer,
        .freq_hz = NewFreq,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };

    if (ledc_timer_config(&LedcTimer) != ESP_OK) {
        return false;
    }

    // Update RAM state
    auto *Fan = static_cast<PWMPersistent *>(Block.PersistentData);
    Fan->PWMFreq = NewFreq;

    return true;
}

// Clamps the new duty (specified in %, valid range 0-100), converts it to the LEDC duty
// value and applies it to the fan's PWM channel. The block field stores the CLAMPED value,
// so read-back always matches what was applied.
bool OnPWMDutyChange(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t DataLen)
{
    if (DataLen != sizeof(uint32_t)) return false;
    uint32_t NewDuty = *static_cast<const uint32_t *>(Data);

    // Clamp value to 0-100%
    if (NewDuty > 100)
        NewDuty = 100;

    // Calculate duty cycle: (new_duty / 100) * 1023 computed in one 64-bit step
    // (ESP32-only file), so no precision is lost to early truncation.
    uint32_t Duty = (uint32_t)(((int64_t)NewDuty * 1023) / 100);

    // Map the block instance to its channel
    int FanSlot = FanIndex(Block);
    if (FanSlot < 0)
        return false;
    ledc_channel_t Channel = (FanSlot == 0) ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, Channel, Duty);
    bool Applied = (ledc_update_duty(LEDC_LOW_SPEED_MODE, Channel) == ESP_OK);

    // Only commit the block state once the hardware actually took the value; otherwise
    // the field would claim a duty the fan is not running.
    if (Applied)
    {
        auto *Fan = static_cast<PWMVolatile *>(Block.VolatileData);
        Fan->Duty = NewDuty;
    }
    return Applied;
}