#include <Wire.h>

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include <ESP32Servo.h>
#else
//#include <Servo.h>
#endif

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include <FastLED.h>
CRGB *BeginLED(uint16_t Length, uint16_t Pin)
{
    CRGB *leds = new CRGB[Length];
    switch (Pin)
    {
    case 0:
        FastLED.addLeds<NEOPIXEL, 0>(leds, Length);
        break;
    case 1:
        FastLED.addLeds<NEOPIXEL, 1>(leds, Length);
        break;
    case 2:
        FastLED.addLeds<NEOPIXEL, 2>(leds, Length);
        break;
    case 3:
        FastLED.addLeds<NEOPIXEL, 3>(leds, Length);
        break;
    case 4:
        FastLED.addLeds<NEOPIXEL, 4>(leds, Length);
        break;
    case 5:
        FastLED.addLeds<NEOPIXEL, 5>(leds, Length);
        break;
    case 6:
        FastLED.addLeds<NEOPIXEL, 7>(leds, Length);
        break;
    case 8:
        FastLED.addLeds<NEOPIXEL, 8>(leds, Length);
        break;
    case 9:
        FastLED.addLeds<NEOPIXEL, 9>(leds, Length);
        break;
    case 10:
        FastLED.addLeds<NEOPIXEL, 10>(leds, Length);
        break;
    default:
        break;
        Serial.println("LED pin missing: " + String(Pin));
    }

    return leds;
};

void EndLED(CRGB *Address)
{
    delete[] Address;
};

#else
#include <Adafruit_NeoPixel.h>
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