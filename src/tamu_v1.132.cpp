#include <Arduino.h>
#include <Adafruit_LSM6DS3TRC.h>

Adafruit_LSM6DS3TRC lsm6ds3trc;

uint32_t LastTime = 0;
uint32_t CurrentTime = 0;
uint32_t DeltaTime = 0;

#include "ClassHeader.h"

#include "Basic\Base.h"
#include "Basic\Hardware.h"
#include "Basic\Memory.h"
#include "Basic\TypeMath.h"
#include "Basic\Math.h"

#include "Core\ValueEnums.h"
#include "Core\Enums.h"
#include "Core\ObjectList.h"
// #include "Core\Map.h"

#include "Data\ByteArray.h"
#include "Data\Colour.h"
#include "Data\Vector2D.h"
#include "Data\Coord2D.h"

#include "Core\ID.h"
#include "Core\IDList.h"
#include "Core\Message.h"
#include "Core\Chirp.h"
ChirpClass Chirp = ChirpClass(); // Bluetooth/Serial

#include "Core\BaseClass.h"
#include "Core\Variable.h"
#include "Core\Register.h"

RegisterClass Objects;
//inputs? accel/button/sensor
ObjectList<> Programs; // Ex: Emotes
ObjectList<> Routines; // Ex: Update positions, color blends
ObjectList<> Outputs;  // Ex: Render

#include "Object\Port.h"
#include "Object\Board.h"
BoardClass Board;

#include "Animation\Float.h"
#include "Animation\Vector2D.h"
#include "Animation\Coord2D.h"
#include "Animation\Colour.h"

#include "Program\Operation.h"
#include "Program\Program.h"

#include "Object\Fan.h"

#include "Variable\Texture1D.h"
#include "Variable\LEDSegment.h"
#include "Object\LEDStrip.h"

#include "Variable\Geometry2D.h"
#include "Variable\Texture2D.h"
#include "Variable\Shape2D.h"
#include "Object\Display.h"

#include "Function\BaseClass.h"
#include "Function\Other.h" //Unrelated to messages
#include "Function\Base.h"
#include "Function\Save.h"
#include "Function\Values.h"

void setup()
{
    MemoryStartup();
    NotificationStartup();
    Serial.begin((long)115200);

    DefaultSetup();
    /*
    File Root = SPIFFS.open("/", "r");
    String File = Root.getNextFileName();
    while (File.length() > 0)
    {
        // Serial.println(File);
        ByteArray Data = ReadFromFile(File.substring(1));
        Message M = Message(Data);
        // Chirp.SendNow(M);
        Message *Response = M.Run();
        Chirp.Send(*Response);
        delete Response;

        File = Root.getNextFileName();
    }
    Root.close();
    */
   
    Chirp.Begin(Board.BTName);
    Chirp.Send(Objects.ContentDebug());
    TimeUpdate();
    Board.BootTime = CurrentTime;

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
};

void loop()
{
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

    Chirp.Communicate();
    // Update shapes and such

    // Serial.println("P");
    for (int32_t Index = 0; Index < Programs.Length; Programs.Iterate(&Index))
        Programs[Index]->Run();
    // Serial.println("R");
    for (int32_t Index = 0; Index < Routines.Length; Routines.Iterate(&Index))
        Routines[Index]->Run();
    // Serial.println("O");
    for (int32_t Index = 0; Index < Outputs.Length; Outputs.Iterate(&Index))
        Outputs[Index]->Run();
    // Serial.println("F");

    FastLED.show();

    TimeUpdate();
    Board.UpdateLoopTime();
};

/*TODO:
KEY FEATURES:
!!! Merge for both boards
Built-in button
Servo
Accelerometer + filtering/integrating
Run function ... implement run on/off/startup/once
Operations without inbetween variables? Type determinition and instant execution?
- use ValueAs for the type (it's operation only, and that is a separate type)

App Usability/parity
Animations & Operations

ADJUSTMENTS:
(ID)List, register make unsigned, for unknown index separate function
Spread out bluetooth sending to prevent lag, use indication for max possible speed?
Sort out reference searching
Saving takes forever

EXTRA:
Edit rendering to allow all-display filters
Extra button / connection -> storageless start
Create default configs (in app)

BUGS:
Separate saving breaks it
Deletion with program running (or Board/Board component)
Incorrect wraping of LED strip
Only one pin valid with leds
*/

// 20.07.2024 Started over >:)
// 28.09.2024 Lit up display again ;D
// 10.11.2024 Change to how setups and types work, partial input for new structure, flags
// 20.12.2024 Separated modules
// 24.01.2025 IDList, Module enums
// 28.01.2025 Types sorted by size
// 06.02.2025 App is kinda working :D
// 07.02.2025 Virtual Deconstructors, Setup and Run
// 09.02.2025 Updated Variable
// 10.02.2025 It loaded!
// 14.02.2025 Program, Some operations, Some animations
// 25.02.2025 Removed references, therefore solved module-reference synchronization :D
// 02.03.2025 Tamu v2.0 arrived! Switch to BLE, FastLED
// 03.03.2025 BLE both ways!