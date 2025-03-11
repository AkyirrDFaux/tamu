#include <FastLED.h>
CRGB *BeginLED(uint16_t Length, uint16_t Pin);
void EndLED(CRGB *Address);

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

CRGB *BeginLED(uint16_t Length, uint16_t Pin)
{
    CRGB *leds = new CRGB[Length];
    switch (Pin)
    {
    case 0:
        FastLED.addLeds<NEOPIXEL, 0>(leds, Length);
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

#include <Adafruit_LSM6DS3TRC.h>

Adafruit_LSM6DS3TRC lsm6ds3trc;

/*
    //Accelerometer setup
    Wire.begin(4,5,10000);
    if (!lsm6ds3trc.begin_I2C(0b1101011))// 0b1101011 or 0b1101010
    {
        Serial.println("Failed to find LSM6DS3TR-C chip");
        while (1)
        {
            delay(10);
        }
    }
*/

/*
//Accelerometer test
sensors_event_t accel;
sensors_event_t gyro;
sensors_event_t temp;
lsm6ds3trc.getEvent(&accel, &gyro, &temp);
Serial.print(String(accel.acceleration.x) + "," + String(accel.acceleration.y) + "," + String(accel.acceleration.z) + ";");
Serial.print(String(gyro.gyro.x) + "," + String(gyro.gyro.y) + "," + String(gyro.gyro.z) + ";");
Serial.println(String(temp.temperature));
*/