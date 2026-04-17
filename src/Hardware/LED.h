#define NOP4() __asm__ volatile("nop; nop; nop; nop;")
#define NOP16() \
    NOP4();     \
    NOP4();     \
    NOP4();     \
    NOP4();
#define NOP32() \
    NOP16();    \
    NOP16();
#define NOP64() \
    NOP16();    \
    NOP16();    \
    NOP16();    \
    NOP16();

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
// #include "led_strip.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"
#include "esp_attr.h"
class LEDDriver
{
public:
    // Handle to the hardware (shared across offsets)
    uint8_t *LEDs = nullptr;
    uint32_t Length = 0;
    uint32_t PinMask = 0;

    // On ESP32-C3, we use the GPIO hardware structure directly
    // W1TS = Write 1 To Set (High)
    // W1TC = Write 1 To Clear (Low)
    volatile uint32_t *SetReg = (volatile uint32_t *)GPIO_OUT_W1TS_REG;
    volatile uint32_t *ClearReg = (volatile uint32_t *)GPIO_OUT_W1TC_REG;

    LEDDriver() {};
    // Compatibility: Length and Pin as uint16_t
    LEDDriver(uint16_t Length, Pin Pin);

    uint8_t *Offset(uint32_t Offset);
    void Show();
    void Stop();
};

LEDDriver::LEDDriver(uint16_t NewLength, Pin Pin)
{
    Length = NewLength;
    LEDs = new uint8_t[Length * 3];
    memset(LEDs, 0, Length * 3);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << Pin.Number),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    this->PinMask = (1 << Pin.Number);
}

void IRAM_ATTR LEDDriver::Show()
{
    if (!LEDs)
        return;

    // Localize variables to the stack (CPU registers) to avoid memory lookups
    uint8_t *pixel = LEDs;
    uint32_t byteLength = Length * 3;
    uint32_t mask = PinMask;

    // Use a Mux to lock the core
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

    portENTER_CRITICAL(&mux);

    while (byteLength--)
    {
        uint8_t channel = *pixel++;

        for (int8_t i = 7; i >= 0; i--)
        {
            if (channel & (1 << i))
            {
                // T1H
                GPIO.out_w1ts.val = mask; // High
                NOP64();
                NOP64();
                NOP16();
                GPIO.out_w1tc.val = mask; // Low
                NOP64();
            }
            else
            {
                // T0H
                GPIO.out_w1ts.val = mask; // High
                NOP16();
                NOP16();
                NOP4();
                GPIO.out_w1tc.val = mask; // Low
                NOP64();
                NOP64();
                NOP16();
            }
        }
    }

    portEXIT_CRITICAL(&mux);
    esp_rom_delay_us(80); // Longer reset for safety
}

void LEDDriver::Stop()
{
    if (LEDs != nullptr)
    {
        delete[] LEDs;
        LEDs = nullptr;
    }
};

uint8_t *LEDDriver::Offset(uint32_t Offset)
{
    return &(LEDs[Offset * 3]);
}

#elif defined BOARD_Valu_v2_0

#define LED_HIGH() (*((volatile uint32_t *)0x40010C10) = (1 << 8))
#define LED_LOW() (*((volatile uint32_t *)0x40010C14) = (1 << 8))

class LEDDriver
{
public:
    uint8_t *LEDs = nullptr;
    uint32_t Length = 0;

    // Hardware registers resolved at construction
    volatile uint32_t *SetReg = nullptr;
    volatile uint32_t *ClearReg = nullptr;
    uint32_t PinMask = 0;

    LEDDriver() {};
    LEDDriver(uint16_t NewLength, Pin pin);

    // Navigation and Data Write
    uint8_t *Offset(uint32_t Offset);

    void Show();
    void Stop();
};

LEDDriver::LEDDriver(uint16_t NewLength, Pin pin)
{
    this->Length = NewLength;
    this->LEDs = new uint8_t[Length * 3];
    memset(LEDs, 0, Length * 3);

    // 1. Initialize the GPIO Hardware using the existing ModeOutput
    HW::ModeOutput(pin);

    // 2. Resolve the registers using our char Port helper
    GPIO_TypeDef *port = GetPort(pin.Port);

    this->PinMask = (1 << pin.Number);
    this->SetReg = &(port->BSHR);
    this->ClearReg = &(port->BCR);
}

void LEDDriver::Stop()
{
    if (LEDs != nullptr)
    {
        delete[] LEDs;
        LEDs = nullptr;
    }
};

uint8_t *LEDDriver::Offset(uint32_t Offset)
{
    // Returns the memory address for a sub-strip
    return &(LEDs[Offset * 3]);
}

// This replaces the standard .show() using your verified timing
void LEDDriver::Show()
{
    if (!LEDs || !SetReg || !ClearReg)
        return;

    // Localize variables to CPU registers for maximum speed
    uint8_t *pixel = LEDs;
    uint32_t byteLength = Length * 3;
    uint32_t mask = PinMask;
    volatile uint32_t *sReg = SetReg;
    volatile uint32_t *cReg = ClearReg;

    __disable_irq();

    while (byteLength--)
    {
        uint8_t channel = *pixel++;
        if (byteLength % 3 == 0 && HW::USB_Ready)
        {
            __enable_irq();
            HW::tud_task(); // Needs keep-alive, transfers won't be active if really used
            __disable_irq();
        }

        for (int8_t i = 7; i >= 0; i--)
        {
            if (channel & (1 << i))
            {
                // --- Logic 1: Target H:700ns (102 nops), L:600ns (87 nops) ---
                *sReg = mask;
                NOP64();
                NOP32();
                NOP4(); // ~100 nops (690ns)
                *cReg = mask;
                NOP64();
                NOP16();
                NOP4(); // ~84 nops (580ns)
            }
            else
            {
                // --- Logic 0: Target H:350ns (51 nops), L:800ns (116 nops) ---
                *sReg = mask;
                NOP32();
                NOP16();
                NOP4(); // ~52 nops (358ns)
                *cReg = mask;
                NOP64();
                NOP32();
                NOP16(); // ~112 nops (772ns)
            }
        }
    }

    __enable_irq();

    // Reset pulse (Latch)
    // HW::SleepMicro(50);
}
#endif

#if defined O_VYSI_V1_0
uint8_t LayoutVysiv1_0[10 * 11]{
    0, 0, 0, 28, 29, 48, 49, 68, 0, 0, 0,
    0, 0, 11, 27, 30, 47, 50, 67, 69, 0, 0,
    0, 10, 12, 26, 31, 46, 51, 66, 70, 82, 0,
    1, 9, 13, 25, 32, 45, 52, 65, 71, 81, 83,
    2, 8, 14, 24, 33, 44, 53, 64, 72, 80, 84,
    3, 7, 15, 23, 34, 43, 54, 63, 73, 79, 85,
    4, 6, 16, 22, 35, 42, 55, 62, 74, 78, 86,
    0, 5, 17, 21, 36, 41, 56, 61, 75, 77, 0,
    0, 0, 18, 20, 37, 40, 57, 60, 76, 0, 0,
    0, 0, 0, 19, 38, 39, 58, 59, 0, 0, 0};
#endif