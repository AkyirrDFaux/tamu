#include "CompileOptions.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
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
#include "Core\FlexArray.h"
#include "Core\ValueTree.h" //Main storage for values
#include "Core\Objects.h"
#include "Core\Register.h"

// Hardware related
#include "Hardware\Base.h"
#include "Hardware\USB.h"
#include "Hardware\Memory.h"
#include "Hardware\I2C.h"
#include "Hardware\LED.h"
// #include "Hardware\Servo.h"
#include "Hardware\Analog&PWM.h"
#include "Hardware\Chirp.h"
#include "Hardware\OLED.h"
ChirpClass Chirp = ChirpClass(); // Bluetooth/Serial

// Programs and operations

#include "Operations\Core.h"
#include "Operations\Convert.h"
#include "Operations\MathBinary.h"
#include "Operations\MathMulti.h"
#include "Operations\MathForm.h"
#include "Operations\Time.h"
#include "Operations\Flow.h"
#include "Operations\Other.h"
#include "Operations\Exec.h"

#include "Object\Program.h"

#include "Object\Board.h"
BoardClass Board(Reference::Global(0, 0, 0));

// Objects
#include "Object\Input.h"
#include "Object\Sensor.h"
#include "Object\Output.h"

#include "Object\I2CDevice.h"

#include "Object\OLED.h"

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

int main()
{
    HW::Init();
    HW::NotificationStartup();
    HW::FlashInit();
    // HW::FlashFormat();

    // ESP_LOGI("MAIN", "Setuping");
    Board.Setup(0); // Initialize devices
    HW::LoadAll();

    // ESP_LOGI("MAIN", "Starting Chirp");
    Result name = Board.Values.Get(Board.Values.Next(0));
    if (name.Length > 0 && name.Type == Types::Text)
        Chirp.Begin(Text((char *)name.Value, name.Length));

    // 2. Update the internal clock
    TimeUpdate();

    Board.Values.Set(&CurrentTime, sizeof(int32_t), Types::Integer, Board.Values.Next(Board.Values.Child(0)), true);

    // ESP_LOGI("MAIN", "Starting");
    /*bool AllFinished = false;
    while (!AllFinished)
    {
        AllFinished = true;
        Chirp.Communicate();

        for (int32_t Index = Objects.Registered - 1; Index >= 0; Index--)
        {
            RegisterEntry &Entry = Objects.Object[Index];
            if (Entry.Info.Period > 0)
            {
                RunInfo Status = Entry.Object->Run(Flags::RunOnStartup);
                if (Status.Period > 0)
                    AllFinished = false;
                Entry.Info = Status;
            }
        }
    }*/
    uint32_t LoopCounter = 0;
    // ESP_LOGI("MAIN", "Entering Loop");
    while (1)
    {
        Chirp.Communicate();
        for (int32_t Index = Objects.Registered - 1; Index >= 0; Index--)
        {
            RegisterEntry &Entry = Objects.Object[Index];
            if (Entry.Info.Period > 0 && (LoopCounter + Entry.Info.Phase) % Entry.Info.Period == 0)
            {
                RunInfo Status = Entry.Object->Run(Flags::RunLoop | Flags::RunOnce);
                Entry.Info = Status;
            }
        }

        LoopCounter++;
        TimeUpdate();
#if defined BOARD_Tamu_v2_0
        HW::Sleep(5);
#endif
    }
};

#if defined BOARD_Tamu_v2_0

// 1. The Task Function
void application_task(void *pvParameters)
{
    // Call your existing main() logic
    // Note: If your main() has an infinite loop, it stays here.
    main();

    // Safety: FreeRTOS tasks must never return.
    // If main() ever exits, we must delete the task.
    vTaskDelete(NULL);
}

extern "C"
{
    void app_main(void)
    {
        // 2. Create the task
        // Name: "app_task"
        // Stack: 8192 bytes (Adjust as needed, C3 is limited)
        // Priority: 5 (Standard priority)
        xTaskCreate(application_task, "app_task", 8192, NULL, 5, NULL);
    };
}
#endif
/*TODO:
KEY FEATURES:
Valu v2.0 - port 0 (pin A14) output
Port type connect blocking
LED strip
Servo
Startup programs

FUTURE:
App Programming view
All-display filters
Experimental paralel LED bit-banging
Multiple boards together

ADJUSTMENTS:
Better compression in storage (Undefined with span of N indexes)
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
// 21.03.2026 New value system (layered, uses paths, no modules), app styling
// 08.04.2026 Corrected saving, read-only, new setup, more operations, functions, commmunication improvements
// 11.04.2026 Large bugfix, ValueTree rewrite, Akyirr on new version, File backup
// 19.04.2026 Many bugfixes, a lot of operations, OLED on Valu v2.0

// Helpful debugging snippet :)
/*#include "esp_log.h"
#include "freertos/FreeRTOS.h"
void checkHeap(const char* label) {
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGD("HEAP","!!! HEAP CORRUPT AT: %s !!!\n", label);
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); } // Stop here so we can see the label
    }
}*/
