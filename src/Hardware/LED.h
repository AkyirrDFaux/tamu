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