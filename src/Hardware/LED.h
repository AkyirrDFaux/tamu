#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include <FastLED.h>

void UpdateLED()
{
    FastLED.show();
}

class LEDDriver
{
public:
    CRGB *LEDs = nullptr;
    LEDDriver() {};
    LEDDriver(uint16_t Length, uint16_t Pin);
    LEDDriver Offset(uint32_t Offset);
    void Write(uint32_t Index, ColourClass Colour);
    void Stop();
};

LEDDriver::LEDDriver(uint16_t Length, uint16_t Pin)
{
    LEDs = new CRGB[Length];
    switch (Pin)
    {
    case 0:
        FastLED.addLeds<NEOPIXEL, 0>(LEDs, Length);
        break;
    case 1:
        FastLED.addLeds<NEOPIXEL, 1>(LEDs, Length);
        break;
    case 2:
        FastLED.addLeds<NEOPIXEL, 2>(LEDs, Length);
        break;
    case 3:
        FastLED.addLeds<NEOPIXEL, 3>(LEDs, Length);
        break;
    case 4:
        FastLED.addLeds<NEOPIXEL, 4>(LEDs, Length);
        break;
    case 5:
        FastLED.addLeds<NEOPIXEL, 5>(LEDs, Length);
        break;
    case 6:
        FastLED.addLeds<NEOPIXEL, 7>(LEDs, Length);
        break;
    case 8:
        FastLED.addLeds<NEOPIXEL, 8>(LEDs, Length);
        break;
    case 9:
        FastLED.addLeds<NEOPIXEL, 9>(LEDs, Length);
        break;
    case 10:
        FastLED.addLeds<NEOPIXEL, 10>(LEDs, Length);
        break;
    default:
        break;
        Serial.println("LED pin missing: " + String(Pin));
    }
};

void LEDDriver::Stop()
{
    delete[] LEDs;
};

LEDDriver LEDDriver::Offset(uint32_t Offset)
{
    LEDDriver OffsetDriver = LEDDriver();
    OffsetDriver.LEDs = &(LEDs[Offset]);
    return OffsetDriver;
}

inline void LEDDriver::Write(uint32_t Index, ColourClass Colour)
{
    LEDs[Index].setRGB(Colour.R, Colour.G, Colour.B);
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

    LEDDriver(){};
    LEDDriver(uint16_t NewLength, uint16_t Pin);

    LEDDriver Offset(uint32_t Offset);
    void Write(uint32_t Index, ColourClass Colour);
    void Show();
    void Stop();
};

LEDDriver::LEDDriver(uint16_t NewLength, uint16_t Pin)
{
    Length = NewLength;
    LEDs = new uint8_t[Length * 3];

    uint32_t PortBase = 0;
    uint8_t PinNumber = 0;

    if (Pin == PA14)
    {
        PortBase = 0x40010800; // Port A
        PinNumber = 14;
        *((volatile uint32_t *)0x40021018) |= (1 << 2);     // GPIOA Clock
        *((volatile uint32_t *)0x40021018) |= (1 << 0);     // AFIO Clock
        *((volatile uint32_t *)0x40010004) |= (0x04000000); // Remap
    }
    else if (Pin == PB1 || Pin == PB8 || Pin == PB10 || Pin == PB11)
    {
        PortBase = 0x40010C00;                          // Port B
        *((volatile uint32_t *)0x40021018) |= (1 << 3); // GPIOB Clock

        if (Pin == PB1)
            PinNumber = 1;
        else if (Pin == PB8)
            PinNumber = 8;
        else if (Pin == PB10)
            PinNumber = 10;
        else if (Pin == PB11)
            PinNumber = 11;
    }

    this->PinMask = (1 << PinNumber);
    this->SetReg = (volatile uint32_t *)(PortBase + 0x10);
    this->ClearReg = (volatile uint32_t *)(PortBase + 0x14);

    // CR Register Selection
    // CRL = 0x00 (Pins 0-7), CRH = 0x04 (Pins 8-15)
    uint32_t crAddr = PortBase + (PinNumber < 8 ? 0x00 : 0x04);
    uint8_t shift = (PinNumber % 8) * 4;

    // Safe Register Update
    volatile uint32_t *regPtr = (volatile uint32_t *)crAddr;
    uint32_t cfg = *regPtr;
    cfg &= ~(0xF << shift);
    cfg |= (0x3 << shift); // 50MHz Out PP
    *regPtr = cfg;
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

    noInterrupts();
    __asm__ volatile("" : : : "memory");

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
    interrupts();
    delayMicroseconds(50);
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