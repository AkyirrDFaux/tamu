#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"

#include "Storage.h"
#include "Log.h"
#include "Core/Functions/Device.h"
#include "Core/Functions/SNDB.h"
#include "Core/Functions/TimeSync.h"
#include "Core/Functions/Memory.h"
#include "Core/Services/StaticMemory.h" // SystemPersistent + the active-flag array sizing

// Function declarations
void LoadAllBackups();
void ReRegisterSubscriptions();
void SubscriptionsTick(uint32_t nowMs);
void ScriptsTick(uint32_t nowMs);
void ScriptsBootLoad();

// Device identity (mandatory, see Core/Functions/Device.h).
extern const DeviceType kDeviceType = DeviceType::Tamu_v2_0A;
// Core (ID assignment, SN registry, time sync), the app link, and both user
// memory services - matching the USE_* build flags so the app shows their views.
extern const uint32_t kCapabilities = Capabilities::Core |
                                        Capabilities::DynamicMemory |
                                        Capabilities::Scripts |
                                        Capabilities::StorageFiles |
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

// The static memory: two flat spaces (Docs/Services/Register.md "System + Static memory
// blocks"). Persistent settings (mirrored 1:1 to .SV) and volatile values, stacked in
// BlockInfo order (lowest BlockType first, instances contiguous per type). The block code
// addresses the fields through these, so the whole space is one compile-time layout.
struct StaticPersistent {
    SystemPersistent system;      // BlockType 0 (special)
    PWMPersistent fan[2];         // BlockType 4
    AccGyrPersistent accgyr;      // BlockType 5
    Vysi1Persistent display[2];   // BlockType 6
};
struct StaticVolatile {
    LEDButtonVolatile ledButton;  // BlockType 3
    PWMVolatile fan[2];           // BlockType 4
    AccGyrVolatile accgyr;        // BlockType 5
    Vysi1Volatile display[2];     // BlockType 6
};
StaticPersistent staticPer = {
    // Name is a fixed 16-char space-padded field (no terminator).
    .system = {.Name = {'T','a','m','u',' ','v','2','.','0','A',' ',' ',' ',' ',' ',' '}, .NetId = 0},
    .fan = {},
    .accgyr = {},
    .display = {},
};
StaticVolatile staticVol;

Vysi1Display Display1(staticVol.display[0], staticPer.display[0]);
Vysi1Display Display2(staticVol.display[1], staticPer.display[1]);

const char* DeviceVersion = "Tamu v2.0A";

// Registry order is load-bearing: the app reconstructs the registry index from the array,
// reconstructs it from type + per-type instance order. Keep the entries grouped by type (see
// FindStaticBlock in Core/Services/RegisterDefs.h).
const StaticBlockDescriptor static_block_registry[] = {
    {&staticVol.ledButton, nullptr, &LEDButton_Schema, "LEDButton"},
    {&staticVol.fan[0], &staticPer.fan[0], &PWM_Schema, "Fan1"},
    {&staticVol.fan[1], &staticPer.fan[1], &PWM_Schema, "Fan2"},
    {&staticVol.accgyr, &staticPer.accgyr, &AccGyr_Schema, "AccGyr"},
    {&staticVol.display[0], &staticPer.display[0], &Vysi1_Schema, "LEDDisplay"},
    {&staticVol.display[1], &staticPer.display[1], &Vysi1_Schema, "LEDDisplay2"}};
const size_t static_block_num = sizeof(static_block_registry) / sizeof(StaticBlockDescriptor);

#include "PWM.h"
#include "Button.h"
#include "AccGyr.h"
#include "LED.h"

LEDDriver LED(3, 0); // both LED strips: pins 3 (Display1) and 0 (Display2), sent in parallel

#include "AppUSB.h"
#include "AppBLE.h"
#include "RSBus.h"



// Main FreeRTOS task: initialises hardware, broadcasts its serial number to find a short address, then runs the main control loop (bus, buttons, IMU, display).
void ApplicationTask(void *pvParameters)
{
PinModeOutput(LED_NOTIFICATION_PIN);
    PinLow(LED_NOTIFICATION_PIN);
    DeviceStatus.ShortAddress = 0;

ESP_LOGI("INIT","b1 storage"); Storage.Init();
ESP_LOGI("INIT","b2 backups"); LoadAllBackups(); // restores name + net-id from .SV too
ScriptsBootLoad();         // loads SCR_XXX scripts flagged load-on-boot (Docs/Services/Script.md)
PreloadVysiLayout();       // Vysi v1.0 layout file -> storage as "LAY_1"
Vysi1BootLayout(Display1); // apply the (restored) layout file to each display
Vysi1BootLayout(Display2);
ESP_LOGI("INIT","b3 appif"); AppInterfaceInit();

    // BLE app link (Nordic UART service); advertised under the device version string.
ESP_LOGI("INIT","b4 ble"); AppBLEInit(DeviceVersion);

ESP_LOGI("INIT","b5 usb"); AppUSBInit(); AppUSBStartTask();

    // Seed PRNG with hardware RNG for CSMA backoff randomisation.
    SeedRand(esp_random());

ESP_LOGI("INIT","b6 rs485"); SetupRS485();
ESP_LOGI("INIT","b7 pwm"); SetupFanPWM();
ESP_LOGI("INIT","b8 imu"); InitLSM6DS3();
LED.Setup();

    PinHigh(LED_NOTIFICATION_PIN);
    PinModeInput(LED_NOTIFICATION_PIN);

    // The LED state is a writable static-block field persisted in the .SV backup;
    // the boot restore writes the RAM field but does not re-run its write trigger, so
    // re-apply it to drive the pin to match the restored value (LED off by default).
    OnLEDStateChange(static_block_registry[0], 3, (const void *)&staticVol.ledButton.LEDState, sizeof(bool));

    // The Tamu is always the core (ID 1): no discovery needed, no button check.
    DeviceStatus.ShortAddress = 1;

    // Re-register the provider side of any restored requester subscriptions now that we
    // have our bus address (the provider table is non-persistent per the docs).
    ReRegisterSubscriptions();

    // Core discover (docs, "Core functions"): the persistent net-ID was restored by
    // LoadAllBackups from .SV (0 is not allowed -> randomly re-generated), then
    // broadcast Core-discover to all cores (3F.1). A response carrying a MATCHING net
    // within 500 ms means this net is claimed twice on the bus -> normal boot is
    // aborted (issue logged; the main loop blinks the error LED) while the core stays
    // reachable via the app link to change the net-ID.
    if (DeviceStatus.NetId == 0 || DeviceStatus.NetId >= 0x3F)
    {
        DeviceStatus.NetId = (uint8_t)(1 + (RawRand() % 61));
    }
    ESP_LOGI("INIT", "core net ID = %u", (unsigned)DeviceStatus.NetId);

    PacketFrame cd;
    PacketConstruct(&cd, ADDR_ALL_CORES,
                    MakeService(ServiceType::Device, 10),
                    NextSystemTrid(ServiceType::Device),
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
    // SN (a stray broadcast) allocates a fresh ID (2) and leaves a bogus
    // entry; AddDevice also replaces any wrong ID already stored for this SN.
    if (SNDB::FindShortID(GetSerialNumber()) != 1)
        SNDB::AddDevice(GetSerialNumber(), 1);

    ReportLog(MakeLog(false, (uint16_t)ServiceType::Device, 0, 0));

    static bool s_identify_prev = false;
    while (1)
    {
        int64_t loopStart = esp_timer_get_time();
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
                OnLEDStateChange(static_block_registry[0], 3, (const void *)&staticVol.ledButton.LEDState, sizeof(bool));
            s_identify_prev = ident;
        }
        if (ident)
            gpio_set_level(LED_NOTIFICATION_PIN, ((DeviceStatus.UptimeMs / 100) & 1) ? 1 : 0);
        }

        ProcessBus();
        SubscriptionsTick(DeviceStatus.UptimeMs); // periodic provider triggers (docs: checked from the main loop)
        ScriptsTick(DeviceStatus.UptimeMs);       // run loaded script programs (Docs/Services/Script.md)
        AppInterfacePump();
        ButtonUpdate();

        // Sample the IMU at its configured output data rate instead of every loop
        // (the sensor registers only change at ODR, so faster reads are wasted bus time).
        {
            static const uint16_t OdrPeriodMs[8] = {80, 38, 19, 10, 5, 2, 1, 1};
            static uint32_t lastImuMs = 0;
            uint8_t odr = staticPer.accgyr.SamplingRate < 8 ? staticPer.accgyr.SamplingRate : 3;
            if (DeviceStatus.UptimeMs - lastImuMs >= OdrPeriodMs[odr]) {
                lastImuMs = DeviceStatus.UptimeMs;
                ReadIMUData();
            }
        }

        // Render the configured render blocks (Vysi1Display, driven by the LEDDisplay
        // static block's Brightness/Offset/RenderBlock fields) and send both LED strips
        // (pins 0,3) IN PARALLEL, tracking each display's achieved refresh rate (FPS,
        // averaged) in its Read-Only Refresh Rate field. The FPS reflects the FULL frame
        // period (loop start to loop start), i.e. the display's real update rate.
        Display1.Render();
        Display2.Render();
        LED.SendParallel(Display1.Buffer, Display2.Buffer, Vysi1Display::LedNum);
        {
            // FPS EMA weights (alpha = 0.1) in 16.16 fixed point; the pair sums to exactly
            // 1.0 so the average does not drift.
            static const Number FpsEmaNew = Number::FromRaw(6554);          // 0.1
            static const Number FpsEmaKeep = Number::FromRaw(65536 - 6554); // 0.9
            static int64_t lastFrameUs = 0;
            if (lastFrameUs != 0)
            {
                // FPS = 1e6 / period_us, kept in 16.16 fixed point (no float).
                int64_t period_us = loopStart - lastFrameUs;
                if (period_us <= 0) period_us = 1;
                Number inst = Number::FromRaw((int32_t)((1000000LL << 16) / period_us));
                Display1.Vol.RefreshRate = Display1.Vol.RefreshRate * FpsEmaKeep + inst * FpsEmaNew;
                Display2.Vol.RefreshRate = Display2.Vol.RefreshRate * FpsEmaKeep + inst * FpsEmaNew;
            }
            lastFrameUs = loopStart;
        }

        // Poll the bus again after the LED bit-bang (which masks interrupts for ~6 ms):
        // it halves the worst-case request->handler latency, which is the dominant term in
        // the TimeSync round-trip asymmetry (the node's clock accuracy).
        ProcessBus();

        Sleep(2); // short heartbeat: BLE request/response latency scales with this loop period
        TimeUpdate();
        // RAW time: the schedule must not be affected by a clock correction (see Tick).
        CoreTimeSync.Tick(TimeFromBoot()); // core syncs ITSELF to the reference core
    }
}

extern "C"
{
    // FreeRTOS entry point: spawns the application task.
    void app_main(void)
    {
        // Core bootloader (B1, Docs/Services/Bootloader.md): re-arm the factory slot so the
        // bootloader runs first on the next reset and can catch the button; it boots this app
        // again when the button is released. Updates only ever write ota_0, so the factory
        // bootloader stays reachable even if this image is broken.
        const esp_partition_t *factory = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
        if (factory != nullptr) esp_ota_set_boot_partition(factory);

        // 16 KB: LoadAllBackups places a MEMORY_BACKUP_CAP-sized buffer on this stack and
        // ProcessBus dispatches full request/response chains recursively beneath it.
        xTaskCreate(ApplicationTask, "app_task", 16384, NULL, 5, NULL);
    };
}
