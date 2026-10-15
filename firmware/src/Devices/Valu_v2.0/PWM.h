#pragma once

// Fan output (Docs/Modules and blocks/Generic system blocks.md): a simple PWM output. The
// Valu has ONE fan channel, PA8, which is TIM1 channel 1 (Docs/Devices.md "Fan PWM output
// PA8"; the previous working release drives the same pin through TIM1 CH1). 32-bit math only
// so no 64-bit helpers are pulled in.

#define FAN_PWM_PERIOD_MAX 65535u

// Auto-reload value for a frequency: period = SystemCoreClock / freq - 1, clamped to the
// 16-bit timer range (the reference release's formula, at the 144 MHz HCLK).
static uint32_t FanPeriodForFreq(uint32_t Freq)
{
    const uint32_t MinFreq = (SystemCoreClock / (FAN_PWM_PERIOD_MAX + 1u)); // lowest representable
    if (Freq < MinFreq) Freq = MinFreq;
    uint32_t Period = (SystemCoreClock / Freq);
    if (Period == 0) Period = 1;
    Period -= 1;
    if (Period > FAN_PWM_PERIOD_MAX) Period = FAN_PWM_PERIOD_MAX;
    return Period;
}

// Applies a new auto-reload value to TIM1 (channel 1 keeps its duty ratio until a duty write).
static void FanApplyPeriod(uint32_t Period)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = {0};
    TIM_TimeBaseStructure.TIM_Prescaler = 0;
    TIM_TimeBaseStructure.TIM_Period = (uint16_t)Period;
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

    FanApplyPeriod(FanPeriodForFreq(StaticPer.Fan[0].PWMFreq));

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

    // Bring the fan up on the DUTY the recall restored, through the same applier a field write
    // uses (the % -> compare-value conversion and the clamp stay in one place, and the field is
    // committed there so a read-back matches what the fan runs). Without this the channel sat at
    // TIM_Pulse = 0 after every power cycle while the block reported the stored duty.
    OnPWMDutyChange(StaticBlockRegistry[1], 1, &StaticPer.Fan[0].PWMDuty, sizeof(uint32_t));
}

// Reconfigures the fan PWM frequency (stored in the block) on TIM1.
bool OnPWMFrequencyChange(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t DataLen)
{
    if (DataLen != sizeof(uint32_t)) return false;
    uint32_t NewFreq = *static_cast<const uint32_t *>(Data);

    // FanPeriodForFreq clamps a frequency below the timer's 16-bit-period minimum, so the
    // frequency the fan would then run differs from the request. Program the clamped value and
    // store THAT value (stored == applied), and refuse the write when the clamp changed the
    // request so the caller is not told a frequency the fan is not on.
    const uint32_t MinFreq = (SystemCoreClock / (FAN_PWM_PERIOD_MAX + 1u));
    uint32_t Applied = (NewFreq < MinFreq) ? MinFreq : NewFreq;
    FanApplyPeriod(FanPeriodForFreq(Applied));

    auto *Fan = static_cast<PWMPersistent *>(Block.PersistentData);
    Fan->PWMFreq = Applied;
    return Applied == NewFreq;
}

// Clamps the new duty (0-100 %), converts it to the timer's compare value and applies it to
// TIM1 CH1. The block field stores the CLAMPED value, so read-back matches what was applied.
bool OnPWMDutyChange(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t DataLen)
{
    if (DataLen != sizeof(uint32_t)) return false;
    uint32_t NewDuty = *static_cast<const uint32_t *>(Data);
    if (NewDuty > 100) NewDuty = 100;

    uint32_t Period = TIM1->ATRLR;
    uint32_t Duty = (NewDuty * Period) / 100u;
    if (Duty > Period) Duty = Period;
    TIM1->CH1CVR = (uint16_t)Duty;

    auto *Fan = static_cast<PWMPersistent *>(Block.PersistentData);
    Fan->PWMDuty = NewDuty;
    return true;
}
