#include <Arduino.h>

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

#include "Core\ID.h"

//Basic data types
#include "Data\ByteArray.h"
#include "Data\Colour.h"
#include "Data\Vector2D.h"
#include "Data\Coord2D.h"
#include "Data\Vector3D.h"

#include "Core\DataList.h"
#include "Core\IDList.h"
#include "Core\Chirp.h"
ChirpClass Chirp = ChirpClass(); // Bluetooth/Serial

#include "Core\BaseClass.h"
#include "Core\Register.h"

RegisterClass Objects;
ObjectList<> Sensors;  // EX: Sensor
ObjectList<> Programs; // Ex: Emotes
ObjectList<> Outputs;  // Ex: Display, Fan, Servo

#include "Object\Port.h"
#include "Object\AccGyr.h"
#include "Object\Input.h"
#include "Object\Board.h"
BoardClass Board;

//Programs
#include "Core\Operation.h"
#include "Core\Program.h"

//Objects
#include "Object\Fan.h"
#include "Object\Servo.h"

#include "Variable\Texture1D.h"
#include "Variable\LEDSegment.h"
#include "Object\LEDStrip.h"

#include "Variable\Geometry2D.h"
#include "Variable\Texture2D.h"
#include "Variable\Shape2D.h"
#include "Object\Display.h"

//Unrelated to messages
#include "Function\Port.h" 
#include "Function\BaseClass.h"
#include "Function\List.h"
#include "Function\Other.h" 

//Messages
#include "Function\Base.h" 
#include "Function\Save.h"
#include "Function\Values.h"

void setup()
{
    ESP32PWM::allocateTimer(0);
	ESP32PWM::allocateTimer(1);
	ESP32PWM::allocateTimer(2);
	ESP32PWM::allocateTimer(3);
    MemoryStartup();
    NotificationStartup();
    Serial.begin((long)115200);

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

    for (int32_t Index = 0; Index < Sensors.Length; Sensors.Iterate(&Index))
        Sensors[Index]->Run();
     //Serial.println("P");
    for (int32_t Index = 0; Index < Programs.Length; Programs.Iterate(&Index))
    {
        if ((Programs[Index]->Flags == RunLoop) || (Programs[Index]->Flags == RunOnce))
            Programs[Index]->Run();
    }
     //Serial.println("O");
    for (int32_t Index = 0; Index < Outputs.Length; Outputs.Iterate(&Index))
        Outputs[Index]->Run();
     //Serial.println("F");

    FastLED.show();

    TimeUpdate();
    Board.UpdateLoopTime();
};

/*TODO:
Filter accelerometer data
More expressions

KEY FEATURES:
Accelerometer filtering

More Operations/Animations
App Usability/parity
Merge for both boards
Memory
App Presets

ADJUSTMENTS:
Register, IDList, DataList, ObjList cleanup
Finish port system (connecting multiple LEDS, I2C)
As (bytearray) checking not working
Spread out bluetooth sending to prevent lag?
Saving takes forever

EXTRA:
Edit rendering to allow all-display filters (part first)
Extra button / connection -> storageless start?
Create default configs (in app)

BUGS:
Separate saving breaks it
Deletion with program running (or Board/Board component)
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