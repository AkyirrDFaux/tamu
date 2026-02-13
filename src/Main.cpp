#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <type_traits>

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

// Core system
#include "Core\ID.h"
#include "Core\Enum.h"
#include "Core\ByteArray.h" //Simple data types (Number, vector, colour, string)
#include "Core\Objects.h"
#include "Core\Register.h"
#include "Core\IDList.h" //For objects (compound/type varying data types)
#include "Core\BaseClass.h"

IDList Sensors;  // EX: Sensor
IDList Programs; // Ex: Emotes
IDList Outputs;  // Ex: Display, Fan, Servo

// Hardware related
#include "Hardware\Base.h"
#include "Hardware\USB.h"
#include "Hardware\Memory.h"
#include "Hardware\I2C.h"
#include "Hardware\LED.h"
#include "Hardware\Servo.h"
#include "Hardware\Analog&PWM.h"
#include "Hardware\Chirp.h"
#include "Hardware\OLED.h"
ChirpClass Chirp = ChirpClass(); // Bluetooth/Serial

// Programs
#include "Object\Operation.h"
#include "Object\Program.h"

// Objects
#include "Object\Port.h"
#include "Object\AccGyr.h"
#include "Object\Input.h"
#include "Object\Sensor.h"
#include "Object\Board.h"
BoardClass Board;

#include "Object\Fan.h"
#include "Object\Servo.h"

// LED strip
#include "Object\Texture1D.h"
#include "Object\LEDSegment.h"
#include "Object\LEDStrip.h"

// Display
#include "Object\Geometry2D.h"
#include "Object\Texture2D.h"
#include "Object\Shape2D.h"
#include "Object\Display.h"

// Unrelated to messages
#include "Function\Port.h"
#include "Function\BaseClass.h"
// Messages
#include "Function\Base.h"
#include "Function\Save.h"
#include "Function\Values.h"

#if defined BOARD_Tamu_v2_0
#include "DefaultSetupTamuv2.0.h" //30kb of instructions :)
#elif defined BOARD_Valu_v2_0
#include "DefaultSetupValuv2.0.h"
#endif

int main()
{
    HW::Init();
    HW::NotificationStartup();

    // MemoryStartup();
    Board.Setup(0);
    DefaultSetup();
    Chirp.Begin(Board.ValueGet<Text>(Board.DisplayName));
#if defined BOARD_Valu_v2_0
    HW::OLED_Init(); // Initialize U8g2
#endif

    TimeUpdate();
    Board.ValueSet<uint32_t>(CurrentTime, Board.BootTime);

    bool AllRun = false;
    while (AllRun == false) // Start Loop
    {
        AllRun = true;
        for (uint32_t Index = 0; Index < Programs.Length; Programs.Iterate(&Index))
        {
            if ((Programs[Index]->Flags == RunOnStartup))
                AllRun &= Programs[Index]->Run();
        }
    }

    while (1) // Main Loop
    {
#if defined BOARD_Valu_v2_0
        u8g2_ClearBuffer(&u8g2);
        u8g2_SetFont(&u8g2, u8g2_font_6x10_tr); // Use a small 'r' font!
        u8g2_DrawStr(&u8g2, 0, 10, "UiiAi");
        u8g2_DrawFrame(&u8g2, 0, 30, 128, 30);
        u8g2_SendBuffer(&u8g2);
#endif

        Chirp.Communicate();

        for (uint32_t Index = 0; Index < Sensors.Length; Sensors.Iterate(&Index))
            Sensors[Index]->Run();

        for (uint32_t Index = 0; Index < Programs.Length; Programs.Iterate(&Index))
        {
            if (((Programs[Index]->Flags == Inactive) == false) && ((Programs[Index]->Flags == RunLoop) || (Programs[Index]->Flags == RunOnce)))
                Programs[Index]->Run();
        }

        for (uint32_t Index = 0; Index < Outputs.Length; Outputs.Iterate(&Index))
            Outputs[Index]->Run();

        UpdateLED();
        TimeUpdate();
        Board.UpdateLoopTime();
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
Saving, switch to littleFS - fallback?

All-display filters
Better LED strip functions

App Programming view
App Presets/blocks

FUTURE:
OLED/TFT Display support
Multiple boards together

ADJUSTMENTS:
use more const, final, inline keywords, pass by reference
Groups instead of favourites?
Flag clarification (Auto -> System = nondeletable, some error flags?)
Value names?... send with separate fcn? or just make it fixed ahead with virtual fcn?

Register, IDList cleanup
Split up individual functions in geometry, possibly combine multiple in one, remove shape2D?
Go over BLE functions, spread out sending/recieving delay, better compression
Operation - prevent recursion, remove unnecessary loops in fuction calls

More optimised ByteArray functions (sequential & multiple(type + object) reading)
RAM saving - Names/Strings only on flash
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