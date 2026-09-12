#include "esp_log.h"
#include "esp_random.h"
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
void ReRegisterSubscriptions();
void SubscriptionsTick(uint32_t nowMs);

// Device identity (mandatory, see Core/Functions/Device.h).
extern const DeviceType kDeviceType = DeviceType::Tamu_v2_0A;
// Core (ID assignment, SN registry, time sync), CLI console, and both user
// memory services - matching the USE_* build flags so the app shows their views.
extern const uint32_t kCapabilities = Capabilities::Core | Capabilities::Cli |
                                        Capabilities::DynamicMemory |
                                        Capabilities::AppInterface |
                                        Capabilities::Subscriptions;

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
ESP_LOGI("INIT","b2 backups"); LoadAllBackups(); // restores name + net-id from STATLOG too
PreloadVysiLayout();       // Vysi v1.0 layout file (layouts/ dir) into storage
ESP_LOGI("INIT","b3 appif"); AppInterfaceInit();

    // BLE app link (Nordic UART service); advertised under the device version string.
ESP_LOGI("INIT","b4 ble"); AppBLEInit(DeviceVersion);

ESP_LOGI("INIT","b5 cli"); StartCLI();

    // Seed PRNG with hardware RNG for CSMA backoff randomisation.
    SeedRand(esp_random());

ESP_LOGI("INIT","b6 rs485"); SetupRS485();
ESP_LOGI("INIT","b7 pwm"); SetupFanPWM();
ESP_LOGI("INIT","b8 imu"); InitLSM6DS3();
LED1.Setup();
LED2.Setup();

    PinHigh(LED_NOTIFICATION_PIN);
    PinModeInput(LED_NOTIFICATION_PIN);

    // The LED state is a writable static-block field persisted in the SYSMEM backup;
    // the boot restore writes the RAM field but does not re-run its write trigger, so
    // re-apply it to drive the pin to match the restored value (LED off by default).
    OnLEDStateChange(static_block_registry[0], 0, (const void *)&LedButton.LEDState, sizeof(bool));

    // The Tamu is always the core (ID 1): no discovery needed, no button check.
    DeviceStatus.ShortAddress = 1;

    // Re-register the provider side of any restored requester subscriptions now that we
    // have our bus address (the provider table is non-persistent per the docs).
    ReRegisterSubscriptions();

    // Core discover (docs, "Core functions"): the persistent net-ID was restored by
    // LoadAllBackups from STATLOG (0 is not allowed -> randomly re-generated), then
    // broadcast Core-discover to all cores (3F.1). A response carrying a MATCHING net
    // within 500 ms means this net is claimed twice on the bus -> normal boot is
    // aborted (issue logged; the main loop blinks the error LED) while the core stays
    // reachable via App/CLI to change the net-ID.
    if (DeviceStatus.NetId == 0 || DeviceStatus.NetId >= 0x3F)
    {
        DeviceStatus.NetId = (uint8_t)(1 + (RawRand() % 61));
    }
    ESP_LOGI("INIT", "core net ID = %u", (unsigned)DeviceStatus.NetId);

    PacketFrame cd;
    PacketConstruct(&cd, ADDR_ALL_CORES,
                    MakeService(ServiceType::Device, 10),
                    MakeService(ServiceType::Device, 10),
                    FLAG_REQACK | FLAG_START | FLAG_STOP,
                    (const uint8_t *)&GetSerialNumber(), sizeof(SerialNumber));
    SendAndVerifyPacket(cd);

    uint32_t cd_end = TimeFromBoot() + 500; // docs: responses expected within 500 ms
    while ((int32_t)(TimeFromBoot() - cd_end) < 0)
    {
        ProcessBus();
        Sleep(10);
    }
    if (CoreCollisionFlag())
        ESP_LOGE("CORE", "Net-ID %u collides with another core - normal boot aborted",
                 (unsigned)DeviceStatus.NetId);

    // Register the core's own serial number as ID 1 (once). Otherwise a Discover of its own
    // SN (CLI self-test or a stray broadcast) allocates a fresh ID (2) and leaves a bogus
    // entry; AddDevice also replaces any wrong ID already stored for this SN.
    if (SNDB::FindShortID(GetSerialNumber()) != 1)
        SNDB::AddDevice(GetSerialNumber(), 1);

    ReportLog(MakeLog(false, (uint16_t)ServiceType::Device, 0, 0));

    static bool s_identify_prev = false;
while (1)
    {
        // Net-ID collision (Core-discover): blink the error LED slowly (~1 Hz) until
        // the net is changed. Takes priority over identify and the LED-button state.
        if (CoreCollisionFlag())
        {
            PinModeOutput(LED_NOTIFICATION_PIN);
            gpio_set_level(LED_NOTIFICATION_PIN, ((DeviceStatus.UptimeMs / 500) & 1) ? 1 : 0);
            s_identify_prev = false; // keep the identify restore logic in sync
        }
        else
        {
        // Identify (Device CID 2): blink the notification LED fast (~5 Hz) while a
        // host asks us to identify ourselves, then re-apply the LED-button state.
        bool ident = DeviceIdentifyActive(DeviceStatus.UptimeMs);
        if (ident != s_identify_prev)
        {
            if (ident)
                PinModeOutput(LED_NOTIFICATION_PIN);
            else
                OnLEDStateChange(static_block_registry[0], 0, (const void *)&LedButton.LEDState, sizeof(bool));
            s_identify_prev = ident;
        }
        if (ident)
            gpio_set_level(LED_NOTIFICATION_PIN, ((DeviceStatus.UptimeMs / 100) & 1) ? 1 : 0);
        }

        ProcessBus();
        SubscriptionsTick(DeviceStatus.UptimeMs); // periodic provider triggers (docs: checked from the main loop)
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
            // FPS = 1e6 / elapsed_us, kept in 16.16 fixed point (no float).
            int64_t elapsed_us = esp_timer_get_time() - rt;
            if (elapsed_us <= 0) elapsed_us = 1;
            Number inst = Number::FromRaw((int32_t)((1000000LL << 16) / elapsed_us));
            Display1.Data.RefreshRate = Display1.Data.RefreshRate * Number::FromRaw(58982) + inst * Number::FromRaw(6553);
        }

        rt = esp_timer_get_time();
        Display2.Render();
        LED2.Send(Display2.Buffer, Vysi1Display::LedNum);
        {
            int64_t elapsed_us = esp_timer_get_time() - rt;
            if (elapsed_us <= 0) elapsed_us = 1;
            Number inst = Number::FromRaw((int32_t)((1000000LL << 16) / elapsed_us));
            Display2.Data.RefreshRate = Display2.Data.RefreshRate * Number::FromRaw(58982) + inst * Number::FromRaw(6553);
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