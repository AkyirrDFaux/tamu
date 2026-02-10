
#if defined BOARD_Tamu_v1_0
#define LED_NOTIFICATION_PIN LED_BUILTIN

#elif defined BOARD_Tamu_v2_0
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
struct Pin
{
    uint8_t Number;
};
const Pin LED_NOTIFICATION_PIN = {2};

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
    void ModeOutput(const Pin &Pin);
    void ModeInput(const Pin &Pin);
    void ModeAnalog(const Pin &Pin);

    bool Read(const Pin &Pin);
    void High(const Pin &Pin);
    void Low(const Pin &Pin);
    uint16_t AnalogRead(const Pin &Pin);

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
    void Init() {};
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

    int32_t GetHeap()
    {
        return ESP.getHeapSize();
    };
    int32_t GetFreeHeap() { return ESP.getHeapSize() - ESP.getFreeHeap(); };

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
    void USB_Send(const ByteArray &Data)
    {
        if (Data.Length == 0 || Data.Array == nullptr)
            return;

        uint32_t sent = 0;
        while (sent < Data.Length)
        {
            // 2. Calculate how much we can send in this "chunk"
            uint32_t available = tud_cdc_write_available();
            uint32_t to_send = (Data.Length - sent > available) ? available : (Data.Length - sent);

            if (to_send > 0)
            {
                tud_cdc_write(Data.Array + sent, to_send);
                sent += to_send;
                tud_cdc_write_flush();
            }

            // 3. The "Reliability" Sleep
            // Give the USB peripheral time to actually move data to the PC.
            // We call tud_task() to keep the USB state machine alive.
            HW::Sleep(1);
            tud_task();
        }
    }

    ByteArray USB_Read()
    {
        uint32_t Length = tud_cdc_available();

        // Check if any bytes are waiting in the RX FIFO
        if (Length > 0)
        {
            ByteArray Data = ByteArray();
            Data.Array = new char[Length];
            Data.Length = Length;
            tud_cdc_read(Data.Array, Data.Length);
            return Data;
        }
        else
            return ByteArray();
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
    int32_t GetMemory() { return 0; };
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