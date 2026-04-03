namespace HW
{

#if defined BOARD_Tamu_v2_0
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
    static adc_oneshot_unit_handle_t adc_handle;
    static bool adc_init_done = false;

    void ModeAnalog(const Pin &Pin)
    {
        if (!adc_init_done)
        {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1, // C3 only has ADC1
                .ulp_mode = ADC_ULP_MODE_DISABLE,
            };
            ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));
            adc_init_done = true;
        }

        // Configure the specific channel for the pin
        // Note: You must map GPIO to ADC_CHANNEL (e.g., GPIO 0 is Channel 0)
        adc_oneshot_chan_cfg_t config = {
            .atten = ADC_ATTEN_DB_12, // 0-3.3V range
            .bitwidth = ADC_BITWIDTH_12,
        };
        // Helper to get channel from GPIO
        adc_channel_t channel;
        adc_unit_t unit;
        adc_oneshot_io_to_channel(Pin.Number, &unit, &channel);
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, channel, &config));
    }

    uint16_t AnalogRead(const Pin &Pin)
    {
        int raw_out;
        adc_channel_t channel;
        adc_unit_t unit;
        adc_oneshot_io_to_channel(Pin.Number, &unit, &channel);
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, channel, &raw_out));
        return (uint16_t)raw_out;
    }

    void ModePWM(const Pin &Pin, uint32_t Frequency = 25000)
    {
        // 1. Timer Config
        // Use the specific Enum for speed_mode to satisfy the C++ type checker
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = Frequency,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = false};
        // Explicitly zero out any hidden flags if the warning persists
        // ledc_timer.flags.output_invert = 0;

        ledc_timer_config(&ledc_timer);

        // 2. Channel Config
        ledc_channel_config_t ledc_channel = {
            .gpio_num = (int)Pin.Number,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
            .flags = {0}};
        // If your IDF version has the .flags struct, initialize it specifically:
        // .flags = { .output_invert = 0 }

        ledc_channel_config(&ledc_channel);
    }

    void PWM(const Pin &Pin, Number Percentage)
    {
        // Convert 16.16 Fixed Point Percentage to 10-bit Duty (0-1023)
        // Formula: (Percentage >> 16) * 1023 / 100
        uint32_t duty = (uint32_t)(((uint64_t)Percentage.Value * 1023) / (100 << 16));

        if (duty > 1023)
            duty = 1023;

        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
#elif defined BOARD_Valu_v2_0

    void ModeAnalog(const Pin &pin)
    {
        GPIO_TypeDef *port = GetPort(pin.Port);

        // 1. Set pin to Analog Input (0x0 in CFGLR/CFGHR)
        // Each pin takes 4 bits in the Configuration Register
        uint32_t shift = (pin.Number % 8) * 4;

        if (pin.Number < 8)
        {
            port->CFGLR &= ~(0xF << shift);
        }
        else
        {
            port->CFGHR &= ~(0xF << shift);
        }

        // 2. Enable ADC1 Clock and Wake up
        // Note: You may also need to enable the clock for the GPIO Port itself
        // (e.g., RCC->APB2PCENR |= RCC_APB2Periph_GPIOA)
        RCC->APB2PCENR |= RCC_APB2Periph_ADC1;
        ADC1->CTLR2 |= ADC_ADON;

        // Calibration (Recommended after power-on)
        ADC1->CTLR2 |= ADC_RSTCAL;
        while (ADC1->CTLR2 & ADC_RSTCAL)
            ;

        ADC1->CTLR2 |= ADC_CAL;
        while (ADC1->CTLR2 & ADC_CAL)
            ;
    }

    uint16_t AnalogRead(const Pin &Pin)
    {
        // Map Pin to ADC Channel (Simplified: PA0=CH0, PA1=CH1...)
        uint8_t channel = Pin.Number;

        // Configure channel sequence
        ADC1->RSQR3 = channel;

        // Start conversion
        ADC1->CTLR2 |= ADC_ADON;

        // Wait for EOC (End of Conversion) flag
        while (!(ADC1->STATR & ADC_EOC))
            ;

        return (uint16_t)ADC1->RDATAR;
    }

    void ModePWM(const Pin &pin, uint32_t Frequency = 25000)
    {
        // 1. Map char Port to Register Address
        GPIO_TypeDef *port = GetPort(pin.Port);

        // 2. Enable Clocks (144MHz usually means APB2 is at full speed)
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
        if (pin.Port == 'A')
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
        if (pin.Port == 'B')
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

        // 3. Enable Timer 1 Clock (Specific to PA8)
        if (pin.Port == 'A' && pin.Number == 8)
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);

        // 4. GPIO Init
        GPIO_InitTypeDef GPIO_InitStructure = {0};
        GPIO_InitStructure.GPIO_Pin = (1 << pin.Number);
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
        GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init(port, &GPIO_InitStructure);

        // 5. Dynamic Period Calculation
        // Formula: (144,000,000 / Frequency) - 1
        // For 25kHz, this results in 5759.
        uint32_t systemClock = 144000000;
        uint32_t period = (systemClock / Frequency) - 1;

        // Safety check for 16-bit timer limits
        if (period > 65535)
            period = 65535;

        TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = {0};
        TIM_TimeBaseStructure.TIM_Prescaler = 0; // High speed, no prescaler
        TIM_TimeBaseStructure.TIM_Period = period;
        TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
        TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
        TIM_TimeBaseInit(TIM1, &TIM_TimeBaseStructure);

        // 6. Channel Init (PWM Mode 1)
        TIM_OCInitTypeDef TIM_OCInitStructure = {0};
        TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
        TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
        TIM_OCInitStructure.TIM_Pulse = 0;
        TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;

        if (pin.Port == 'A' && pin.Number == 8)
        {
            TIM_OC1Init(TIM1, &TIM_OCInitStructure);
            TIM_OC1PreloadConfig(TIM1, TIM_OCPreload_Enable);
        }

        // 7. TIM1 "Main Output Enable" (MOE bit)
        // Essential for Advanced Timers like TIM1/TIM8
        TIM_CtrlPWMOutputs(TIM1, ENABLE);
        TIM_Cmd(TIM1, ENABLE);
    }

    void PWM(const Pin &pin, Number Percentage)
    {
        // Fetch current Auto-Reload Value (Period) from the register
        uint32_t period = TIM1->ATRLR;

        // Convert 16.16 Fixed Point to Duty Cycle
        // Duty = (Percentage.Value * period) / (100 * 65536)
        uint32_t duty = (uint32_t)(((uint64_t)Percentage.Value * period) / (100 << 16));

        if (duty > period)
            duty = period;

        if (pin.Port == 'A' && pin.Number == 8)
        {
            // Write directly to Capture/Compare Register 1
            TIM1->CH1CVR = duty;
        }
    }
#endif
}