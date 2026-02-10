#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include "led_strip.h"

class LEDDriver
{
public:
    // Handle to the hardware (shared across offsets)
    led_strip_handle_t LEDs = nullptr;
    uint32_t OffsetStart = 0;

    LEDDriver() {};
    // Compatibility: Length and Pin as uint16_t
    LEDDriver(uint16_t Length, Pin Pin);

    LEDDriver Offset(uint32_t Offset);
    void Write(uint32_t Index, ColourClass Colour);
    void Show();
    void Stop();
};

LEDDriver::LEDDriver(uint16_t Length, Pin Pin)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = (gpio_num_t)Pin.Number,
        .max_leds = Length,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // ADD THIS
        .flags = {.invert_out = false}};

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .mem_block_symbols = 64,           // ADD THIS (standard for WS2812)
        .flags = {.with_dma = false}};

    // Initialize hardware for this specific pin
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &LEDs);
    if (err != ESP_OK)
    {
        // Log error or handle failure
    }
}

void LEDDriver::Show()
{
    if (LEDs)
    {
        led_strip_refresh(LEDs);
    }
}

void LEDDriver::Stop()
{
    if (LEDs)
    {
        led_strip_del(LEDs);
        LEDs = nullptr;
    }
}

LEDDriver LEDDriver::Offset(uint32_t Offset)
{
    LEDDriver OffsetDriver = LEDDriver();
    OffsetDriver.LEDs = this->LEDs; // Pass the hardware handle
    OffsetDriver.OffsetStart = this->OffsetStart + Offset;
    return OffsetDriver;
}

inline void LEDDriver::Write(uint32_t Index, ColourClass Colour)
{
    if (LEDs)
    {
        // We add the internal offset to the requested index
        led_strip_set_pixel(LEDs, Index + OffsetStart, Colour.R, Colour.G, Colour.B);
    }
}

#elif defined BOARD_Valu_v2_0

// Macros for PB8 - Verified working at 144MHz
#define LED_HIGH() (*((volatile uint32_t *)0x40010C10) = (1 << 8))
#define LED_LOW() (*((volatile uint32_t *)0x40010C14) = (1 << 8))
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