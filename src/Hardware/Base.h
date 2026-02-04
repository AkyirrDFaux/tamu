#ifdef BOARD_Tamu_v1_0
    #define LED_NOTIFICATION_PIN LED_BUILTIN
    #define ON_STATE LOW
    #define OFF_STATE HIGH
#endif
#ifdef BOARD_Tamu_v2_0
    #define LED_NOTIFICATION_PIN 2
    #define ON_STATE LOW
    #define OFF_STATE HIGH
#endif
#ifdef BOARD_Valu_v2_0
    #define LED_NOTIFICATION_PIN PA2
    #define ON_STATE HIGH
    #define OFF_STATE LOW
#endif

void TimeUpdate();
void NotificationBlink(int Amount, int Time);
void NotificationStartup();

inline void TimeUpdate(){
    LastTime = CurrentTime;
    CurrentTime = millis();
    DeltaTime = CurrentTime - LastTime;
};

void NotificationBlink(int Amount, int Time)
{
    for (int Iteration = 0; Iteration < Amount; Iteration++)
    {
        digitalWrite(LED_NOTIFICATION_PIN, ON_STATE);
        delay(Time);
        digitalWrite(LED_NOTIFICATION_PIN, OFF_STATE);
        if (Iteration < Amount - 1)
            delay(Time);
    }
};

void NotificationStartup(){
    pinMode(LED_NOTIFICATION_PIN, OUTPUT);
    digitalWrite(LED_NOTIFICATION_PIN, OFF_STATE);
    delay(200);
    NotificationBlink(2, 200);
};

#include <Wire.h>

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
int32_t GetHeap() { return ESP.getHeapSize(); };
int32_t GetFreeHeap() { return ESP.getHeapSize() - ESP.getFreeHeap(); };

#define VOLTAGE (3.3)
#define ADCRES (1 << 12)
#else
int32_t GetHeap() { return 0; };
int32_t GetFreeHeap() { return 0; };

#define VOLTAGE (3.3)
#define ADCRES (1 << 12)
#endif