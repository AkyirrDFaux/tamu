#include <Arduino.h>
uint32_t LastTime = 0;
uint32_t CurrentTime = 0;
uint32_t DeltaTime = 0;

#include "ClassHeader.h"

//Hardware related
#include "Basic\Base.h"
#include "Basic\Hardware.h"
#include "Basic\Memory.h"
#include "Basic\Number.h"
#include "Basic\MathAndType.h"

//Data types
#include "Data\Colour.h"
#include "Data\Vector2D.h"
#include "Data\Coord2D.h"
#include "Data\Vector3D.h"
#include "Data\ValueEnums.h"

//Core system
#include "Core\Enum.h"
#include "Core\ObjectList.h"
#include "Core\ID.h"
#include "Core\ByteArray.h"
#include "Core\Objects.h"
RegisterClass Objects;

#include "Core\DataList.h" //For simple data types
#include "Core\Register.h"
#include "Core\IDList.h" //For objects (compound/type varying data types)
#include "Core\BaseClass.h"

#include "Core\Chirp.h"
ChirpClass Chirp = ChirpClass(); // Bluetooth/Serial

ObjectList<> Sensors;  // EX: Sensor
ObjectList<> Programs; // Ex: Emotes
ObjectList<> Outputs;  // Ex: Display, Fan, Servo

//Programs
#include "Object\Operation.h"
#include "Object\Program.h"

//Objects
#include "Object\Port.h"
#include "Object\AccGyr.h"
#include "Object\Input.h"
#include "Object\Board.h"
BoardClass Board;

#include "Object\Fan.h"
#include "Object\Servo.h"

//LED strip
#include "Object\Texture1D.h"
#include "Object\LEDSegment.h"
#include "Object\LEDStrip.h"

//Display
#include "Object\Geometry2D.h"
#include "Object\Texture2D.h"
#include "Object\Shape2D.h"
#include "Object\Display.h"

//Unrelated to messages
#include "Function\Port.h" 
//Messages
#include "Function\Base.h" 
#include "Function\Save.h"
#include "Function\Values.h"

#include "DefaultSetup.h" 

void setup()
{
    MemoryStartup();
    NotificationStartup();
    Serial.begin((long)115200);
    Board.Setup(0);

    if (1)
        DefaultSetup();
    else
    {
        File Root = SPIFFS.open("/", "r");
        String File = Root.getNextFileName();
        while (File.length() > 0)
        {
            // Serial.println(File);
            ByteArray Data = ReadFromFile(File.substring(1));
            Run(Data);

            File = Root.getNextFileName();
        }
        Root.close();
    }

    Chirp.Begin(*Board.Values.At<String>(Board.BTName));

    Serial.println(Objects.ContentDebug());
    TimeUpdate();
    *Board.Values.At<uint32_t>(Board.BootTime) = CurrentTime;

    bool AllRun = false;
    while (AllRun == false)
    {
        AllRun = true;
        for (int32_t Index = 0; Index < Programs.Length; Programs.Iterate(&Index))
        {
            if ((Programs[Index]->Flags == RunOnStartup))
                AllRun &= Programs[Index]->Run();
        }
    }
};

void loop()
{
    Chirp.Communicate();
    //Serial.print("C:" + String(millis() - CurrentTime));

    for (int32_t Index = 0; Index < Sensors.Length; Sensors.Iterate(&Index))
        Sensors[Index]->Run();
    //Serial.print(", S:" + String(millis() - CurrentTime));

    //Serial.println("P");
    for (int32_t Index = 0; Index < Programs.Length; Programs.Iterate(&Index))
    {
        if (((Programs[Index]->Flags == Inactive) == false) && ((Programs[Index]->Flags == RunLoop) || (Programs[Index]->Flags == RunOnce)))
            Programs[Index]->Run();
    }
    //Serial.print(", P:" + String(millis() - CurrentTime));

    //Serial.println("O");
    for (int32_t Index = 0; Index < Outputs.Length; Outputs.Iterate(&Index))
        Outputs[Index]->Run();
    //Serial.println("L");
    //Serial.print(", O:" + String(millis() - CurrentTime));

    FastLED.show(); //around 6s
    //Serial.println(", L:" + String(millis() - CurrentTime));

    TimeUpdate();
    //Serial.println(DeltaTime);
    Board.UpdateLoopTime();
};

/*TODO:
KEY FEATURES:
Crash (Delete/Create) Safety
Saving, switch to littleFS - fallback?

All-display filters

App Programming view
App Presets/blocks

FUTURE:
Neopixel/FastLED switch define, ESP specific switches
OLED/TFT Display support
Multiple boards together

ADJUSTMENTS:
use final keyword
Groups instead of favourites? 
Flag clarification (Auto -> System = nondeletable, some error flags?)
Value names?... send with separate fcn? or just make it fixed ahead?

Register, IDList, DataList, ObjList cleanup (possibly make datalist data oriented, maybe delete objlist and merge to register)
Split up individual functions in geometry, possibly combine multiple in one
Go over BLE functions, spread out sending/recieving delay, better compression
Operation - prevent recursion

RAM saving - Names/Strings only on flash, F() macro
BUGS:
Incorrect wraping of LED strip
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
// 04.09.2025 App Improvements
// 08.09.2025 Sorted out operations
// 13.09.2025 New port/driver system + Servo + Inputs
// 18.10.2025 DataList replaced Variables -> multi-variable objects
// 19.10.2025 Movement of eye using angular velocity
// 17.12.2025 Split types, app on Windows
// 22.12.2025 Fixed-point replacement, -40% loop time!
// 31.12.2025 Custom gyroscope driver
// 01.01.2026 Setup update
// 02.01.2026 Display layer-based rendering, -30% loop time!
// 13.01.2026 Nested operations
// xx.0x.2026 Improved port and bus system, module attach/detach events