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
#include "Data\Values.h"

// Core system
#include "Core\Reference.h"
#include "Core\Enum.h"
#include "Core\ByteArray.h"       //Core functions
#include "Core\ByteArrayValues.h" //Set & Get
#include "Core\Objects.h"
#include "Core\Register.h"
#include "Core\BaseClass.h"

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

#include "Object\Board.h"
BoardClass Board(Reference::Global(0, 0, 0));

// Objects
#include "Object\Input.h"
#include "Object\Sensor.h"
#include "Object\I2CDevice.h"

// #include "Object\Fan.h"
// #include "Object\Servo.h"

// LED strip
// #include "Object\Texture1D.h"
// #include "Object\LEDSegment.h"
// #include "Object\LEDStrip.h"
#include "Object\Display.h"

// Unrelated to messages
#include "Function\Port.h"
#include "Function\BaseClass.h"
// Messages
#include "Function\Base.h"
#include "Function\Values.h"
// #include "Function\Save.h"

#if defined BOARD_Tamu_v2_0
#include "DefaultSetupTamuv2.0.h"
#elif defined BOARD_Valu_v2_0
#include "DefaultSetupValuv2.0.h"
#endif

int main()
{
    HW::Init();
    HW::NotificationStartup();

    // HW::FlashInit();
    // HW::FlashFormat();

    ESP_LOGI("MAIN", "Setuping");
    Board.Setup(Reference::Global(0, 0, 0)); // Initialize devices
    DefaultSetup();

    ESP_LOGI("MAIN", "Starting Chirp");
    Chirp.Begin(Board.ValueGet<Text>({0,0}));

    TimeUpdate();
    Board.ValueSetup<int32_t>(CurrentTime, {0, 1});

    ESP_LOGI("MAIN", "Starting");
    bool AllFinished = false;
    while (!AllFinished)
    {
        AllFinished = true;
        Chirp.Communicate();

        for (int32_t Index = Objects.Registered - 1; Index >= 0; Index--)
        {
            RegisterEntry &Entry = Objects.Object[Index];

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
    ESP_LOGI("MAIN", "Entering Loop");
    while (1)
    {
        Chirp.Communicate();

        for (int32_t Index = Objects.Registered - 1; Index >= 0; Index--)
        {
            RegisterEntry &Entry = Objects.Object[Index];

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
        HW::Sleep(10);
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
Operations and programs
Saving
Sensors
OLED
Fan, Servo
Compile options
App backup (json?)

FUTURE:
App Programming view
App Presets, blocks
All-display filters
Better LED strip functions
Experimental paralel LED bit-banging
Multiple boards together

ADJUSTMENTS:
use more const, final, inline keywords, pass by reference, overall optimalizaton
Chirp spread out sending/recieving delay, better compression/buffering
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
// 21.03.2025 New value system (layered, uses paths, no modules), app styling