#pragma once

// Fan output (Docs/Modules and blocks/Generic system blocks.md): a simple PWM output. The
// Valu has ONE fan channel, PA8, which is TIM1 channel 1 (Docs/Devices.md "Fan PWM output
// PA8"; the previous working release drives the same pin through TIM1 CH1). 32-bit math only
// so no 64-bit helpers are pulled in.

#define FAN_PWM_PERIOD_MAX 65535u

// Auto-reload value for a frequency: period = SystemCoreClock / freq - 1, clamped to the
// 16-bit timer range (the reference release's formula, at the 144 MHz HCLK).
static uint32_t FanPeriodForFreq(uint32_t freq)
{
    const uint32_t min_freq = (SystemCoreClock / (FAN_PWM_PERIOD_MAX + 1u)); // lowest representable
    if (freq < min_freq) freq = min_freq;
    uint32_t period = (SystemCoreClock / freq);
    if (period == 0) period = 1;
    period -= 1;
    if (period > FAN_PWM_PERIOD_MAX) period = FAN_PWM_PERIOD_MAX;
    return period;
}

// Applies a new auto-reload value to TIM1 (channel 1 keeps its duty ratio until a duty write).
static void FanApplyPeriod(uint32_t period)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = {0};
    TIM_TimeBaseStructure.TIM_Prescaler = 0;
    TIM_TimeBaseStructure.TIM_Period = (uint16_t)period;
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM1, &TIM_TimeBaseStructure);
}

// Configures TIM1 CH1 (PA8) as the fan PWM, at the frequency stored in the block.
void SetupFanPWM()
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_TIM1, ENABLE);

    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    FanApplyPeriod(FanPeriodForFreq(staticPer.fan[0].PWMFreq));

    TIM_OCInitTypeDef TIM_OCInitStructure = {0};
    TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_Pulse = 0;
    TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC1Init(TIM1, &TIM_OCInitStructure);
    TIM_OC1PreloadConfig(TIM1, TIM_OCPreload_Enable);

    // TIM1 is an advanced timer: its outputs are only driven with the Main Output Enable bit.
    TIM_CtrlPWMOutputs(TIM1, ENABLE);
    TIM_Cmd(TIM1, ENABLE);
}

// Reconfigures the fan PWM frequency (stored in the block) on TIM1.
bool OnPWMFrequencyChange(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t data_len)
{
    if (data_len != sizeof(uint32_t)) return false;
    uint32_t new_freq = *static_cast<const uint32_t *>(data);

    FanApplyPeriod(FanPeriodForFreq(new_freq));

    auto *fan = static_cast<PWMPersistent *>(block.PersistentData);
    fan->PWMFreq = new_freq;
    return true;
}

// Clamps the new duty (0-100 %), converts it to the timer's compare value and applies it to
// TIM1 CH1. The block field stores the CLAMPED value, so read-back matches what was applied.
bool OnPWMDutyChange(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t data_len)
{
    if (data_len != sizeof(uint32_t)) return false;
    uint32_t new_duty = *static_cast<const uint32_t *>(data);
    if (new_duty > 100) new_duty = 100;

    uint32_t period = TIM1->ATRLR;
    uint32_t duty = (new_duty * period) / 100u;
    if (duty > period) duty = period;
    TIM1->CH1CVR = (uint16_t)duty;

    auto *fan = static_cast<PWMVolatile *>(block.VolatileData);
    fan->Duty = new_duty;
    return true;
}
