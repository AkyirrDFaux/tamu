#if defined BOARD_Tamu_v1_0
#define LED_NOTIFICATION_PIN LED_BUILTIN

#elif defined BOARD_Tamu_v2_0
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
const Pin LED_NOTIFICATION_PIN = {2,0};
const Pin INVALID_PIN = {255,0};

#elif defined BOARD_Valu_v2_0
#include "ch32v20x.h"
GPIO_TypeDef *GetPort(char portChar)
{
    switch (portChar)
    {
    case 'A':
        return GPIOA;
    case 'B':
        return GPIOB;
    case 'C':
        return GPIOC;
    case 'D':
        return GPIOD;
    default:
        return GPIOA; // Fallback
    }
}
const Pin INVALID_PIN = {0, 0};
const Pin LED_NOTIFICATION_PIN = {2, 'A'};
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

    // RAM
    int32_t GetRAM();
    int32_t GetFreeRAM();

    // Flash
    // bool FlashSave(const ValueTree &Data);
    // ByteArray FlashLoad(IDClass ID);

    // USB
    void USB_Init();
    void USB_Send(const FlexArray &Data);
    FlexArray USB_Read();
    uint8_t CRC8(const uint8_t *Data, size_t Length); // Standard CRC-8 (Maxim/Dallas)

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

        USB_Init();
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

    void ModeInputPullDown(const Pin &Pin)
    {
        gpio_reset_pin((gpio_num_t)Pin.Number);
        gpio_set_direction((gpio_num_t)Pin.Number, GPIO_MODE_INPUT);

        gpio_pullup_dis((gpio_num_t)Pin.Number);
        gpio_pulldown_en((gpio_num_t)Pin.Number);
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

    int32_t GetRAM()
    {
        // Total DRAM available
        return (int32_t)heap_caps_get_total_size(MALLOC_CAP_8BIT);
    }

    int32_t GetFreeRAM()
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
    void SystemInit(void);
}

namespace HW
{
    void Init()
    {
        SystemInit();
        // Start SysTick
        SystemCoreClockUpdate();
        tick_multiplier = SystemCoreClock / 1000;

        SysTick->CTLR = 0;
        SysTick->SR = 0;
        SysTick->CNT = 0;
        SysTick->CMP = 0xFFFFFFFFFFFFFFFFULL;

        SysTick->CTLR = 0xF;
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

        USB_Init();
    }

    bool IsValidPin(const Pin &Pin)
    {
        return Pin.Port != 0;
    };

    // Helper to enable clock based on char
    void EnablePortClock(char portChar)
    {
        if (portChar == 'A')
            RCC->APB2PCENR |= RCC_APB2Periph_GPIOA;
        else if (portChar == 'B')
            RCC->APB2PCENR |= RCC_APB2Periph_GPIOB;
        else if (portChar == 'C')
            RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
        else if (portChar == 'D')
            RCC->APB2PCENR |= RCC_APB2Periph_GPIOD;
    }

    void ModeOutput(const Pin &pin)
    {
        GPIO_TypeDef *port = GetPort(pin.Port);
        EnablePortClock(pin.Port);

        // Special case for PA14 (usually SWCLK)
        if (pin.Port == 'A' && pin.Number == 14)
        {
            RCC->APB2PCENR |= RCC_APB2Periph_AFIO;
            AFIO->PCFR1 |= (1 << 24); // Disable SWD to free the pin
        }

        uint32_t shift = (pin.Number % 8) * 4;
        if (pin.Number < 8)
        {
            port->CFGLR &= ~(0xF << shift);
            port->CFGLR |= (0x3 << shift); // Output 50MHz, Push-Pull
        }
        else
        {
            port->CFGHR &= ~(0xF << shift);
            port->CFGHR |= (0x3 << shift);
        }
    }

    void ModeInput(const Pin &pin)
    {
        GPIO_TypeDef *port = GetPort(pin.Port);
        EnablePortClock(pin.Port);

        uint32_t shift = (pin.Number % 8) * 4;
        if (pin.Number < 8)
        {
            port->CFGLR &= ~(0xF << shift);
            port->CFGLR |= (0x4 << shift); // Floating Input
        }
        else
        {
            port->CFGHR &= ~(0xF << shift);
            port->CFGHR |= (0x4 << shift);
        }
    }

    void ModeInputPullDown(const Pin &pin)
    {
        GPIO_TypeDef *port = GetPort(pin.Port);
        EnablePortClock(pin.Port);

        uint32_t shift = (pin.Number % 8) * 4;
        // Mode 00 (Input), CNF 10 (Pull-up/down) = 0x8
        if (pin.Number < 8)
        {
            port->CFGLR &= ~(0xF << shift);
            port->CFGLR |= (0x8 << shift);
        }
        else
        {
            port->CFGHR &= ~(0xF << shift);
            port->CFGHR |= (0x8 << shift);
        }

        // ODR = 0 for Pull-Down, ODR = 1 for Pull-Up
        port->OUTDR &= ~(1 << pin.Number);
    }

    bool Read(const Pin &pin)
    {
        return (GetPort(pin.Port)->INDR & (1 << pin.Number)) != 0;
    }

    void High(const Pin &pin)
    {
        // BSHR is the "Bit Set High Register"
        GetPort(pin.Port)->BSHR = (1 << pin.Number);
    }

    void Low(const Pin &pin)
    {
        // BCR is the "Bit Clear Register"
        GetPort(pin.Port)->BCR = (1 << pin.Number);
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
    int32_t GetRAM() { return 20 * 1024; };
    int32_t GetFreeRAM()
    {
        char top;
        return &top - reinterpret_cast<char *>(sbrk(0));
    };
#endif

    void NotificationBlink(int Amount, int Time)
    {
        HW::ModeOutput(LED_NOTIFICATION_PIN);
        for (int Iteration = 0; Iteration < Amount; Iteration++)
        {
            HW::Low(LED_NOTIFICATION_PIN); // On
            HW::Sleep(Time);
            HW::High(LED_NOTIFICATION_PIN); // Off
            if (Iteration < Amount - 1)
                HW::Sleep(Time);
        }
        HW::ModeInput(LED_NOTIFICATION_PIN);
    };
    void NotificationStartup()
    {
        HW::ModeOutput(LED_NOTIFICATION_PIN);
        HW::High(LED_NOTIFICATION_PIN); // Inverted logic
        NotificationBlink(2, 200);
    };

}

inline void TimeUpdate()
{
    LastTime = CurrentTime;
    CurrentTime = HW::Now();
    DeltaTime = CurrentTime - LastTime;
};