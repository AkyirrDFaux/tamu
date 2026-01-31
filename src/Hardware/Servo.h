#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include <ESP32Servo.h>
#else
// Placeholder
class Servo
{
public:
    void write(int Value) {};
    void setPeriodHertz(int Freq) {};
    void attach(int Pin, int Min = 500, int Max = 2500) {};
    void detach(){};
};
#endif