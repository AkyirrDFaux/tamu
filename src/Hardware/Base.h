#if defined BOARD_Tamu_v1_0
#define LED_NOTIFICATION_PIN LED_BUILTIN

#elif defined BOARD_Tamu_v2_0
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
struct Pin
{
    uint8_t Number;
};
const Pin LED_NOTIFICATION_PIN = {2};
const Pin INVALID_PIN = {255};

#elif defined BOARD_Valu_v2_0
#include "ch32v20x.h"
struct Pin
{
    GPIO_TypeDef *Port;
    uint16_t Number;
};
const Pin INVALID_PIN = {nullptr, 0};
const Pin LED_NOTIFICATION_PIN = {GPIOA, 2};
#endif
namespace HW
{
    void Init();
    // Pins
    bool IsValidPin(const Pin &Pin);
    void ModeOutput(const Pin &Pin);
    void ModeInput(const Pin &Pin);
    void ModeAnalog(const Pin &Pin);
    void ModePWM(const Pin &Pin);

    bool Read(const Pin &Pin);
    void High(const Pin &Pin);
    void Low(const Pin &Pin);
    uint16_t AnalogRead(const Pin &Pin);
    void PWM(const Pin &Pin, Number Percentage);

    // Time
    uint32_t Now();
    void Sleep(uint32_t ms);
    void SleepMicro(uint32_t us);

    // Memory
    int32_t GetMemory();
    int32_t GetFreeMemory();

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#define VOLTAGE (3.3)
#define ADCRES (1 << 12)
    void Init()
    {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
    };

    bool IsValidPin(const Pin &Pin)
    {
        return Pin.Number != INVALID_PIN.Number;
    };
    void ModeOutput(const Pin &Pin)
    {
        gpio_reset_pin((gpio_num_t)Pin.Number);
        gpio_set_direction((gpio_num_t)Pin.Number, GPIO_MODE_OUTPUT);
    }

    void ModeInput(const Pin &Pin)
    {
        gpio_reset_pin((gpio_num_t)Pin.Number);
        gpio_set_direction((gpio_num_t)Pin.Number, GPIO_MODE_INPUT);
    }

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

    bool Read(const Pin &Pin)
    {
        return gpio_get_level((gpio_num_t)Pin.Number);
    }

    void High(const Pin &Pin)
    {
        gpio_set_level((gpio_num_t)Pin.Number, 1);
    }

    void Low(const Pin &Pin)
    {
        gpio_set_level((gpio_num_t)Pin.Number, 0);
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

    uint32_t Now()
    {
        return (uint32_t)(esp_timer_get_time() / 1000);
    }

    void Sleep(uint32_t ms)
    {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    void SleepMicro(uint32_t us)
    {
        esp_rom_delay_us(us);
    }

    int32_t GetMemory()
    {
        // Total DRAM available
        return (int32_t)heap_caps_get_total_size(MALLOC_CAP_8BIT);
    }

    int32_t GetFreeMemory()
    {
        // Current free heap
        return (int32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    }
#elif defined BOARD_Valu_v2_0
#define VOLTAGE (3.3)
#define ADCRES (1 << 12)
}
static uint64_t tick_multiplier = 0;

extern "C"
{
#include "tusb.h"
    void SystemInit(void);
    void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
    void USB_LP_CAN1_RX0_IRQHandler(void) { tud_int_handler(0); }
    // Called when device is mounted
    void tud_mount_cb(void) {}
    // Called when device is unmounted
    void tud_umount_cb(void) {}
    // Called when usb bus is suspended
    void tud_suspend_cb(bool remote_wakeup_en) {}
    // Called when usb bus is resumed
    void tud_resume_cb(void) {}
    void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
    {
        if (dtr)
        {
            // Terminal connected
        }
    }

    // Called when CDC receives new data
    void tud_cdc_rx_cb(uint8_t itf)
    {
        // Optional: wake up a task to read data
    }
}
namespace HW
{
    // Helper: Standard CRC-8 (Maxim/Dallas)
    uint8_t crc8(const uint8_t *data, size_t len)
    {
        uint8_t crc = 0x00;
        for (size_t i = 0; i < len; i++)
        {
            crc ^= data[i];
            for (int j = 0; j < 8; j++)
            {
                if (crc & 0x80)
                    crc = (crc << 1) ^ 0x31;
                else
                    crc <<= 1;
            }
        }
        return crc;
    }

    void USB_Send(const ByteArray &Data)
    {
        if (Data.Length == 0 || Data.Array == nullptr)
            return;

        const uint32_t MAX_DATA = 60;
        uint32_t offset = 0;

        while (offset < Data.Length)
        {
            static uint8_t __attribute__((aligned(4))) packet[64];
            memset(packet, 0, 64);

            uint8_t to_copy = (uint8_t)((Data.Length - offset > MAX_DATA) ? MAX_DATA : Data.Length - offset);

            packet[0] = 0xFA;
            packet[2] = to_copy; // Length byte
            memcpy(packet + 3, Data.Array + offset, to_copy);
            packet[3 + to_copy] = 0xBF;

            // Calculate CRC on [Len + Data]
            // This includes packet[2] through the end of the data payload
            packet[1] = crc8(packet + 2, to_copy + 1);

            uint32_t timeout = 10000;
            while (tud_cdc_write_available() < 64 && timeout > 0)
            {
                tud_task();
                timeout--;
            }

            tud_cdc_write(packet, 64);
            tud_cdc_write_flush();
            offset += to_copy;
            tud_task();
        }
    }
    // Static buffer to survive between USB_Read() calls
    static uint8_t rx_buffer[128];
    static uint32_t rx_ptr = 0;

    ByteArray USB_Read()
    {
        // 1. Pull everything available from TinyUSB into our local ring buffer
        uint32_t count = tud_cdc_available();
        if (count > 0)
        {
            // Prevent overflow
            if (rx_ptr + count > 128)
                rx_ptr = 0;
            tud_cdc_read(rx_buffer + rx_ptr, count);
            rx_ptr += count;
        }

        // 2. Process our local buffer for a 64-byte frame
        while (rx_ptr >= 64)
        {
            if (rx_buffer[0] == 0xFA)
            {
                uint8_t receivedCrc = rx_buffer[1];
                uint8_t dataLen = rx_buffer[2];

                if (dataLen <= 60)
                {
                    // Check CRC
                    if (crc8(rx_buffer + 2, dataLen + 1) == receivedCrc)
                    {
                        // Check Footer
                        if (rx_buffer[3 + dataLen] == 0xBF)
                        {

                            // SUCCESS: Construct return object
                            ByteArray Data;
                            Data.Length = dataLen;
                            Data.Array = new char[dataLen];
                            memcpy(Data.Array, rx_buffer + 3, dataLen);

                            // Shift buffer by 64
                            memmove(rx_buffer, rx_buffer + 64, rx_ptr - 64);
                            rx_ptr -= 64;
                            return Data;
                        }
                    }
                }
                // If header found but check failed, slide 1 to find next 0xFA
                memmove(rx_buffer, rx_buffer + 1, rx_ptr - 1);
                rx_ptr--;
            }
            else
            {
                // No header at 0, slide 1
                memmove(rx_buffer, rx_buffer + 1, rx_ptr - 1);
                rx_ptr--;
            }
        }

        return ByteArray(); // Nothing valid found yet
    }
    void Init()
    {
        SystemInit();
        // Start SysTick
        SystemCoreClockUpdate(); // Ensure this global variable is actually correct
        tick_multiplier = SystemCoreClock / 1000;

        SysTick->CTLR = 0;
        SysTick->SR = 0;
        SysTick->CNT = 0;
        SysTick->CMP = 0xFFFFFFFFFFFFFFFFULL;

        SysTick->CTLR = 0xF;
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

        // USB (default communication option)
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div3);
        RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);

        RCC_APB1PeriphResetCmd(RCC_APB1Periph_USB, ENABLE);
        HW::Sleep(1); // Reset timer
        RCC_APB1PeriphResetCmd(RCC_APB1Periph_USB, DISABLE);

        tusb_init();

        NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 1);
        NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);

        (EXTEN->EXTEN_CTR) |= EXTEN_USBD_PU_EN; // Pullup D+
    }

    bool IsValidPin(const Pin &Pin)
    {
        return Pin.Port != nullptr;
    };

    void ModeOutput(const Pin &Pin)
    {
        if (Pin.Port == GPIOA && Pin.Number == 14)
        {
            RCC->APB2PCENR |= RCC_APB2Periph_AFIO; // Power AFIO
            AFIO->PCFR1 |= (1 << 24);              // Disable SWD, keep JTAG-DP
        }
        uint32_t Shift = (Pin.Number % 8) * 4;
        if (Pin.Number < 8)
        {
            Pin.Port->CFGLR &= ~(0xF << Shift);
            Pin.Port->CFGLR |= (0x3 << Shift); // Output 50MHz, Push-Pull
        }
        else
        {
            uint32_t ShiftH = ((Pin.Number - 8) % 8) * 4;
            Pin.Port->CFGHR &= ~(0xF << ShiftH);
            Pin.Port->CFGHR |= (0x3 << ShiftH);
        }
    }

    void ModeInput(const Pin &Pin)
    {
        uint32_t Shift = (Pin.Number % 8) * 4;
        if (Pin.Number < 8)
        {
            Pin.Port->CFGLR &= ~(0xF << Shift);
            Pin.Port->CFGLR |= (0x4 << Shift); // Floating Input
        }
        else
        {
            uint32_t ShiftH = ((Pin.Number - 8) % 8) * 4;
            Pin.Port->CFGHR &= ~(0xF << ShiftH);
            Pin.Port->CFGHR |= (0x4 << ShiftH);
        }
    }

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

    bool Read(const Pin &Pin)
    {
        return (Pin.Port->INDR & (1 << Pin.Number)) != 0;
    }

    void High(const Pin &Pin)
    {
        Pin.Port->BSHR = (1 << Pin.Number);
    }

    void Low(const Pin &Pin)
    {
        Pin.Port->BCR = (1 << Pin.Number);
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

    uint32_t Now()
    {
        return (uint32_t)(SysTick->CNT / tick_multiplier);
    }

    void Sleep(uint32_t ms)
    {
        uint32_t start = Now();
        while ((Now() - start) < ms)
            ;
    }

    void SleepMicro(uint32_t us)
    {
        // At 144MHz, a multiplier of ~48 is a good starting point for a 3-instruction loop.
        uint32_t iterations = us * 48;
        __asm__ volatile(
            "1: addi %0, %0, -1\n" // RISC-V: Add immediate -1 to the register
            "   bnez %0, 1b\n"     // RISC-V: Branch to '1' if register is Not Equal to Zero
            : "+r"(iterations)     // Input/Output
            :                      // No other inputs
            : "memory"             // Barriers memory to prevent compiler reordering
        );
    }

    extern "C" char *sbrk(int incr);
    int32_t GetMemory() { return 20 * 1024; };
    int32_t GetFreeMemory()
    {
        char top;
        return &top - reinterpret_cast<char *>(sbrk(0));
    };
#endif
}

inline void TimeUpdate()
{
    LastTime = CurrentTime;
    CurrentTime = HW::Now();
    DeltaTime = CurrentTime - LastTime;
};
void NotificationBlink(int Amount, int Time)
{
    for (int Iteration = 0; Iteration < Amount; Iteration++)
    {
        HW::High(LED_NOTIFICATION_PIN);
        HW::Sleep(200);
        HW::Low(LED_NOTIFICATION_PIN);
        if (Iteration < Amount - 1)
            HW::Sleep(200);
    }
};
void NotificationStartup()
{
    HW::ModeOutput(LED_NOTIFICATION_PIN);
    HW::Low(LED_NOTIFICATION_PIN);
    NotificationBlink(2, 200);
};