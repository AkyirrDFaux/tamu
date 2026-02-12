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

    void ModePWM(const Pin &Pin)
    {
        // 1. Timer Config (25kHz)
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 25000,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = false // ADD THIS
        };
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
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD // ADD THIS
        };
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

    void ModeAnalog(const Pin &Pin)
    {
        // 1. Set pin to Analog Input (0x0 in CFGLR/CFGHR)
        uint32_t shift = (Pin.Number % 8) * 4;
        if (Pin.Number < 8)
        {
            Pin.Port->CFGLR &= ~(0xF << shift);
        }
        else
        {
            Pin.Port->CFGHR &= ~(0xF << shift);
        }

        // 2. Enable ADC1 Clock and Reset
        RCC->APB2PCENR |= RCC_APB2Periph_ADC1;
        ADC1->CTLR2 |= ADC_ADON; // Wake up ADC

        // Calibration (Optional but recommended once at boot)
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

#define PWM_25KHZ 5759

    void ModePWM(const Pin &Pin)
    {
        // 1. Enable AFIO Clock (REQUIRED for Alternate Functions like PWM)
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

        // 2. Enable Port Clock
        if (Pin.Port == GPIOA)
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
        if (Pin.Port == GPIOB)
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

        // 3. Enable Timer Clock
        if (Pin.Port == GPIOA && Pin.Number == 8)
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);

        // 4. Configure Pin Mode
        // We must convert your index '8' into a bitmask: (1 << 8) = 256
        GPIO_InitTypeDef GPIO_InitStructure = {0};
        GPIO_InitStructure.GPIO_Pin = (1 << Pin.Number);
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
        GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init(Pin.Port, &GPIO_InitStructure);

        // 5. Timer Time Base
        TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = {0};
        TIM_TimeBaseStructure.TIM_Prescaler = 0;
        TIM_TimeBaseStructure.TIM_Period = PWM_25KHZ;
        TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
        TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
        TIM_TimeBaseInit(TIM1, &TIM_TimeBaseStructure);

        // 6. Channel Configuration
        TIM_OCInitTypeDef TIM_OCInitStructure = {0};
        TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
        TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
        TIM_OCInitStructure.TIM_Pulse = 0;
        TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;

        if (Pin.Port == GPIOA && Pin.Number == 8)
        {
            TIM_OC1Init(TIM1, &TIM_OCInitStructure);
            TIM_OC1PreloadConfig(TIM1, TIM_OCPreload_Enable);
        }

        // 7. THE CRITICAL FIX: TIM1 "Main Output Enable"
        // Without this, TIM1 will count, but the pin will stay disconnected.
        TIM_CtrlPWMOutputs(TIM1, ENABLE);

        // Enable Timer
        TIM_Cmd(TIM1, ENABLE);
    }

    void PWM(const Pin &Pin, Number Percent)
    {
        uint32_t Duty = (uint32_t)(((uint64_t)Percent.Value * PWM_25KHZ) / (100 << 16));

        if (Duty > PWM_25KHZ)
            Duty = PWM_25KHZ;

        if (Pin.Port == GPIOA && Pin.Number == 8)
        {
            // Use the register directly to be fast
            TIM1->CH1CVR = Duty;
        }
    }
#endif
}