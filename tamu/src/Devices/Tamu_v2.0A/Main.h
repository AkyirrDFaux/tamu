#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"

#include "Storage.h"
#include "Log.h"
#include "Core/Functions/Device.h"
#include "Core/Functions/SNDB.h"
#include "Core/Functions/TimeSync.h"
#include "Core/Functions/Memory.h"

// Function declarations
void LoadAllBackups();

// Device identity (mandatory, see Core/Functions/Device.h).
extern const DeviceType kDeviceType = DeviceType::Tamu_v2_0A;
// Core (ID assignment, SN registry, time sync), CLI console, and both user
// memory services - matching the USE_* build flags so the app shows their views.
extern const uint32_t kCapabilities = Capabilities::Core | Capabilities::Cli |
                                       Capabilities::DynamicMemory |
                                       Capabilities::KeyedMemory;

// Reads the factory MAC from eFuse as the 14-byte serial number (cached).
const SerialNumber &GetSerialNumber()
{
    static SerialNumber sn;
    static bool init = false;
    if (!init)
    {
        esp_efuse_read_field_blob(ESP_EFUSE_MAC_FACTORY, &sn, 48);
        init = true;
    }
    return sn;
}


#include "Base.h"
#include "Blocks/Button.h"
#include "Blocks/PWM.h"
#include "Blocks/AccGyr.h"
#include "Blocks/Vysi1Display.h"

LEDButtonStruct LedButton;
PWMStruct Fan1;
PWMStruct Fan2;
AccGyrStruct AccGyr;
Vysi1Display Display1;
Vysi1Display Display2;

const char* DeviceVersion = "Tamu v2.0A";

const StaticBlockDescriptor static_block_registry[] = {
    {&LedButton, &LEDButton_Schema, "LEDButton"},
    {&Fan1, &PWM_Schema, "Fan1"},
    {&Fan2, &PWM_Schema, "Fan2"},
    {&AccGyr, &AccGyr_Schema, "AccGyr"},
    {&Display1.Data, &Vysi1_Schema, "LEDDisplay"},
    {&Display2.Data, &Vysi1_Schema, "LEDDisplay2"}};
const size_t static_block_num = sizeof(static_block_registry) / sizeof(StaticBlockDescriptor);

#include "PWM.h"
#include "Button.h"
#include "AccGyr.h"
#include "LED.h"

LEDDriver LED1(3);
LEDDriver LED2(0);

#include "AppUSB.h"
#include "AppBLE.h"
#include "CLI/Entry.h"
#include "CLI/Handler.h"
#include "RSBus.h"

// Main FreeRTOS task: initialises hardware, broadcasts its serial number to find a short address, then runs the main control loop (bus, buttons, IMU, display).
void ApplicationTask(void *pvParameters)
{
PinModeOutput(LED_NOTIFICATION_PIN);
    PinLow(LED_NOTIFICATION_PIN);
    DeviceStatus.ShortAddress = 0;

ESP_LOGI("INIT","b1 storage"); Storage.Init();
ESP_LOGI("INIT","b2 backups"); LoadAllBackups();
LoadPersistedDeviceName(); // persisted name active before BLE advertising starts
PreloadVysiLayout();       // Vysi v1.0 layout file (layouts/ dir) into storage
ESP_LOGI("INIT","b3 appif"); AppInterfaceInit();

    // BLE app link (Nordic UART service); advertised under the device version string.
ESP_LOGI("INIT","b4 ble"); AppBLEInit(DeviceVersion);

ESP_LOGI("INIT","b5 cli"); StartCLI();

ESP_LOGI("INIT","b6 rs485"); SetupRS485();
ESP_LOGI("INIT","b7 pwm"); SetupFanPWM();
ESP_LOGI("INIT","b8 imu"); InitLSM6DS3();
LED1.Setup();
LED2.Setup();

    PinHigh(LED_NOTIFICATION_PIN);
    PinModeInput(LED_NOTIFICATION_PIN);

    // The Tamu is always the core (ID 1): no discovery needed, no button check.
    DeviceStatus.ShortAddress = 1;

    // Register the core's own serial number as ID 1 (once). Otherwise a Discover of its own
    // SN (CLI self-test or a stray broadcast) allocates a fresh ID (2) and leaves a bogus
    // entry; AddDevice also replaces any wrong ID already stored for this SN.
    if (SNDB::FindShortID(GetSerialNumber()) != 1)
        SNDB::AddDevice(GetSerialNumber(), 1);

    ReportLog(MakeLog(false, (uint16_t)ServiceType::Device, 0, 0));

while (1)
    {
        ProcessBus();
        AppInterfacePump();
        ButtonUpdate();
        ReadIMUData();

        // Render the configured render block (Vysi1Display, driven by the LEDDisplay
        // static block's Brightness/Offset/RenderBlock fields) to both LED strips (pins
        // 0,3) and track each display's achieved refresh rate (FPS, averaged) in its
        // Read-Only Refresh Rate field.
        int64_t rt = esp_timer_get_time();
        Display1.Render();
        LED1.Send(Display1.Buffer, Vysi1Display::LedNum);
        {
            Number inst = N(1000000.0f) / N((float)(esp_timer_get_time() - rt));
            Display1.Data.RefreshRate = Display1.Data.RefreshRate * N(0.9f) + inst * N(0.1f);
        }

        rt = esp_timer_get_time();
        Display2.Render();
        LED2.Send(Display2.Buffer, Vysi1Display::LedNum);
        {
            Number inst = N(1000000.0f) / N((float)(esp_timer_get_time() - rt));
            Display2.Data.RefreshRate = Display2.Data.RefreshRate * N(0.9f) + inst * N(0.1f);
        }

        Sleep(2); // short heartbeat: BLE request/response latency scales with this loop period
        TimeUpdate();
        TimeSync.Tick(DeviceStatus.UptimeMs);
    }
}

extern "C"
{
    // FreeRTOS entry point: spawns the application task.
    void app_main(void)
    {
        // 16 KB: LoadAllBackups places a MEMORY_BACKUP_CAP-sized buffer on this stack and
        // ProcessBus dispatches full request/response chains recursively beneath it.
        xTaskCreate(ApplicationTask, "app_task", 16384, NULL, 5, NULL);
    };
}