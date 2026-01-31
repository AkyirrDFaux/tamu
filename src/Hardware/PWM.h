#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include <ESP32Servo.h>
typedef ESP32PWM PWMDriver;
#else
// Placeholder
class PWMDriver
{
public:
    void attachPin(int Pin, int Freq = 25000, int Res = 8) {};
    void detachPin(int Pin){};
    void writeScaled(float Value) {};
};
#endif