#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <type_traits>
#include <initializer_list>

uint32_t LastTime = 0;
uint32_t CurrentTime = 0;
uint32_t DeltaTime = 0;

#include "ClassHeader.h"

// Data types
#include "Data\Text.h"
#include "Data\Number.h"
#include "Data\Colour.h"
#include "Data\Vector2D.h"
#include "Data\Coord2D.h"
#include "Data\Vector3D.h"
#include "Data\ValueEnums.h"

struct Pin
{
    uint8_t Number;
    char Port = 0;
};

// Core system
#include "Core\Reference.h"
#include "Core\Enum.h"
#include "Core\ByteArray.h"       //Core functions
#include "Core\ByteArrayValues.h" //Set & Get
#include "Core\Objects.h"
#include "Core\Register.h"
#include "Core\BaseClass.h"

// IDList Sensors;  // EX: Sensor
// IDList Programs; // Ex: Emotes
// IDList Outputs;  // Ex: Display, Fan, Servo

// Hardware related
#include "Hardware\Base.h"
#include "Hardware\USB.h"
// #include "Hardware\Memory.h"
#include "Hardware\I2C.h"
#include "Hardware\LED.h"
// #include "Hardware\Servo.h"
#include "Hardware\Analog&PWM.h"
#include "Hardware\Chirp.h"
#include "Hardware\OLED.h"
ChirpClass Chirp = ChirpClass(); // Bluetooth/Serial

// Programs
// #include "Object\Operation.h"
// #include "Object\Program.h"

// Objects
#include "Object\Bus.h"
// #include "Object\AccGyr.h"
// #include "Object\Input.h"
// #include "Object\Sensor.h"
// #include "Object\OLED.h"
#include "Object\Board.h"
BoardClass Board(Reference(0, 0, 0));

// #include "Object\Fan.h"
// #include "Object\Servo.h"

// LED strip
// #include "Object\Texture1D.h"
// #include "Object\LEDSegment.h"
// #include "Object\LEDStrip.h"

// Display
// #include "Object\Geometry2D.h"
// #include "Object\Texture2D.h"
// #include "Object\Shape2D.h"
#include "Object\Display.h"

// Unrelated to messages
#include "Function\Port.h"
#include "Function\BaseClass.h"
// Messages
#include "Function\Base.h"
#include "Function\Values.h"
// #include "Function\Save.h"

#if defined BOARD_Tamu_v2_0
// #include "DefaultSetupTamuv2.0.h" //30kb of instructions :)
#elif defined BOARD_Valu_v2_0
#include "DefaultSetupValuv2.0.h"
#endif

int main()
{
    HW::Init();
    HW::NotificationStartup();

    // HW::FlashInit();
    // HW::FlashFormat();

    // DefaultSetup();
    Chirp.Begin("Test");
    // Chirp.Begin(Board.ValueGet<Text>(Board.DisplayName));

    TimeUpdate();
    Board.ValueSet<int32_t>(CurrentTime, {0, 1});
    
    bool AllFinished = false;
    while (!AllFinished)
    {
        AllFinished = true;
        Chirp.Communicate();

        for (int32_t i = Objects.Registered - 1; i >= 0; i--)
        {
            RegisterEntry &Entry = Objects.Object[i];

            // Execute if Startup is set and not Inactive
            if (!(Entry.Info.Flags == Flags::Inactive) && (Entry.Info.Flags == Flags::RunOnStartup))
            {
                // If any object returns false, the whole startup phase continues
                if (!Entry.Object->Run())
                {
                    AllFinished = false;
                }
            }
        }
    }

    uint32_t LoopCounter = 0;

    while (1)
    {
        Chirp.Communicate();
        HW::Sleep(10);

        for (int32_t i = Objects.Registered - 1; i >= 0; i--)
        {
            RegisterEntry &Entry = Objects.Object[i];

            if (Entry.Info.Flags == Flags::Inactive)
                continue;

            if (Entry.Info.Flags == Flags::RunOnce || ((Entry.Info.RunTiming > 0) && (LoopCounter % Entry.Info.RunTiming == 0)))
            {
                bool Finished = Entry.Object->Run();

                // Only auto-reset the RunOnce trigger bit
                if (Finished && Entry.Info.Flags == Flags::RunOnce)
                    Entry.Info.Flags -= Flags::RunOnce;
            }
        }

        LoopCounter++;
        TimeUpdate();
    }
};

#if defined BOARD_Tamu_v2_0
extern "C"
{
    void app_main(void) { main(); };
}
#endif
/*TODO:
KEY FEATURES:
Crash (Delete/Create) Safety
Saving

All-display filters
Better LED strip functions

App Programming view
App Presets/blocks

Finish OLED menu
FUTURE:
Multiple boards together

ADJUSTMENTS:
use more const, final, inline keywords, pass by reference

Flag clarification (Auto -> System = nondeletable, some error flags?)
- Groups instead of favourites
- refresh rates for loop runs
Value names?... send with separate fcn? or just make it fixed ahead with virtual fcn?

Register, IDList, ByteArray cleanup
- More optimised ByteArray functions (sequential & multiple(type + object) reading)
Combine Texture, Geometry into Shape2D, Combine Input and Sensor (Input), Combine Fan and Servo (Output)
Go over user functions
Chirp spread out sending/recieving delay, better compression/buffering
Operation - prevent recursion, remove unnecessary loops in fuction calls, add more functions
Names in RAM/ only on flash / no names (compile option)
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
// 22.01.2026 Improved port and bus system, module attach/detach events, removed ObjectList
// 25.01.2026 DataList adapted into ByteArray, -4kb RAM on ~70 Objects
// 31.01.2026 SensorClass created (Analog, NTC and LDR)
// 03.02.2026 Valu v2.0 arrived!
// 05.02.2026 Valu v2.0 LED output
// 07.02.2026 Manual VTable + Destroy Switch
// 08.02.2026 De-arduino-ified Valu v2.0
// 10.02.2026 Chirp over UART
// 12.02.2026 De-arduino-ified Tamu v2.0
// 13.02.2026 Valu v2.0 OLED menu test
// 14.02.2026 Custom saving in flash