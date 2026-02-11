#define NOP4() __asm__ volatile("nop; nop; nop; nop;")
#define NOP16() \
    NOP4();     \
    NOP4();     \
    NOP4();     \
    NOP4();
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
static portMUX_TYPE my_mux = portMUX_INITIALIZER_UNLOCKED;

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

    LEDDriver Offset(uint32_t Offset);
    void Write(uint32_t Index, ColourClass Colour);
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

void IRAM_ATTR LEDDriver::Show() {
    if (!LEDs) return;

    // Localize variables to the stack (CPU registers) to avoid memory lookups
    uint8_t *pixel = LEDs;
    uint32_t byteLength = Length * 3;
    uint32_t mask = PinMask;

    // Use a Mux to lock the core
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    
    portENTER_CRITICAL(&mux);

    while (byteLength--) {
        uint8_t channel = *pixel++;
        
        // Unrolling the bit-loop can sometimes help, but let's try 
        // a tight loop first with manual register addresses
        for (int8_t i = 7; i >= 0; i--) {
            if (channel & (1 << i)) {
                // T1H
                GPIO.out_w1ts.val = mask; // High
                NOP64(); NOP64(); NOP16(); 
                GPIO.out_w1tc.val = mask; // Low
                NOP64(); 
            } else {
                // T0H
                GPIO.out_w1ts.val = mask; // High
                NOP16(); NOP16(); NOP4();
                GPIO.out_w1tc.val = mask; // Low
                NOP64(); NOP64(); NOP16();
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

LEDDriver LEDDriver::Offset(uint32_t Offset)
{
    LEDDriver OffsetDriver = LEDDriver();
    OffsetDriver.LEDs = &(LEDs[Offset * 3]);
    return OffsetDriver;
}

inline void LEDDriver::Write(uint32_t Index, ColourClass Colour)
{
    // Neopixels are GRB
    if (LEDs)
    {
        LEDs[Index * 3] = Colour.G;
        LEDs[Index * 3 + 1] = Colour.R;
        LEDs[Index * 3 + 2] = Colour.B;
    }
}

#elif defined BOARD_Valu_v2_0

// Macros for PB8 - Verified working at 144MHz
#define LED_HIGH() (*((volatile uint32_t *)0x40010C10) = (1 << 8))
#define LED_LOW() (*((volatile uint32_t *)0x40010C14) = (1 << 8))

class LEDDriver
{
public:
    uint8_t *LEDs = nullptr;
    uint32_t Length = 0;

    // Hardware registers
    volatile uint32_t *SetReg = nullptr;
    volatile uint32_t *ClearReg = nullptr;
    uint32_t PinMask = 0;

    LEDDriver() {};
    LEDDriver(uint16_t NewLength, Pin &Pin);

    LEDDriver Offset(uint32_t Offset);
    void Write(uint32_t Index, ColourClass Colour);
    void Show();
    void Stop();
};

LEDDriver::LEDDriver(uint16_t NewLength, Pin &Pin)
{
    Length = NewLength;
    LEDs = new uint8_t[Length * 3];

    HW::ModeOutput(Pin);

    this->PinMask = (1 << Pin.Number);
    this->SetReg = &(Pin.Port->BSHR);
    this->ClearReg = &(Pin.Port->BCR);
}

void LEDDriver::Stop()
{
    if (LEDs != nullptr)
    {
        delete[] LEDs;
        LEDs = nullptr;
    }
};

LEDDriver LEDDriver::Offset(uint32_t Offset)
{
    LEDDriver OffsetDriver = LEDDriver();
    OffsetDriver.LEDs = &(LEDs[Offset * 3]);
    return OffsetDriver;
}

inline void LEDDriver::Write(uint32_t Index, ColourClass Colour)
{
    // Neopixels are GRB
    if (LEDs)
    {
        LEDs[Index * 3] = Colour.G;
        LEDs[Index * 3 + 1] = Colour.R;
        LEDs[Index * 3 + 2] = Colour.B;
    }
}

// This replaces the standard .show() using your verified timing
void LEDDriver::Show()
{
    if (!LEDs || !SetReg)
        return;

    uint8_t *Pixel = LEDs;
    uint16_t ByteLength = Length * 3;

    while (ByteLength--)
    {
        uint8_t Channel = *Pixel++;
        for (int8_t Index = 7; Index >= 0; Index--)
        {
            if (Channel & (1 << Index))
            {
                *SetReg = PinMask; // HIGH
                NOP64();
                NOP16();
                *ClearReg = PinMask; // LOW
                NOP64();
            }
            else
            {
                *SetReg = PinMask; // HIGH
                NOP16();
                NOP4();
                *ClearReg = PinMask; // LOW
                NOP64();
                NOP64();
            }
        }
    }
    HW::SleepMicro(50);
}

#else
#include <Adafruit_NeoPixel.h>

class LEDDriver
{
public:
    int16_t OffsetStart = 0;
    Adafruit_NeoPixel *LEDs;
    LEDDriver() {};
    LEDDriver(uint16_t Length, uint16_t Pin);
    LEDDriver Offset(uint32_t Offset);
    void Write(uint32_t Index, ColourClass Colour);
    void Stop();
};

LEDDriver::LEDDriver(uint16_t Length, uint16_t Pin)
{
    LEDs = new Adafruit_NeoPixel(Length, Pin, NEO_GRB + NEO_KHZ800);
    LEDs->begin();
    LEDs->clear();
    LEDs->show();
};

void LEDDriver::Stop()
{
    delete LEDs;
};

LEDDriver LEDDriver::Offset(uint32_t Offset)
{
    LEDDriver OffsetDriver = LEDDriver();
    OffsetDriver.LEDs = LEDs;
    OffsetDriver.OffsetStart = Offset;
    return OffsetDriver;
}

inline void LEDDriver::Write(uint32_t Index, ColourClass Colour)
{
    LEDs->setPixelColor((uint16_t)(Index + OffsetStart), Colour.R, Colour.G, Colour.B);
}
#endif

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