#ifdef BOARD_Tamu_v1_0
    #define LED_NOTIFICATION_PIN LED_BUILTIN
#endif
#ifdef BOARD_Tamu_v2_0
    #define LED_NOTIFICATION_PIN 2
#endif
#ifdef BOARD_Valu_v2_0
    #define LED_NOTIFICATION_PIN 0
#endif

void TimeUpdate();
void NotificationBlink(int Amount, int Time);
void NotificationStartup();

void TimeUpdate(){
    LastTime = CurrentTime;
    CurrentTime = millis();
    DeltaTime = CurrentTime - LastTime;
};

void NotificationBlink(int Amount, int Time)
{
    for (int Iteration = 0; Iteration < Amount; Iteration++)
    {
        digitalWrite(LED_NOTIFICATION_PIN, LOW);
        delay(Time);
        digitalWrite(LED_NOTIFICATION_PIN, HIGH);
        if (Iteration < Amount - 1)
            delay(Time);
    }
};

void NotificationStartup(){
    pinMode(LED_NOTIFICATION_PIN, OUTPUT);
    digitalWrite(LED_NOTIFICATION_PIN, HIGH);
    delay(200);
    NotificationBlink(2, 200);
};

#include <Wire.h>

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
int32_t GetHeap() { return ESP.getHeapSize(); };
int32_t GetFreeHeap() { return ESP.getHeapSize() - ESP.getFreeHeap(); };
#else
int32_t GetHeap() { return 0; };
int32_t GetFreeHeap() { return 0; };


#endif