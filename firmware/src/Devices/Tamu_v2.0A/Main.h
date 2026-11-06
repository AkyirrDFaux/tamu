#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
// The factory image's boot budget: the numbers and the decision live there, the storage in
// BootBudget.h. This app refunds the budget once it has proved it is alive (see the main loop).
#include "Bootloader/BootBudget.h"
#include "Bootloader/BootDecision.h"

#include "Core/Functions/AppInterface.h"
#include "Storage.h"
#include "Log.h"
#include "Core/Functions/Device.h"
#include "Core/Functions/SNDB.h"
#include "Core/Functions/TimeSync.h"
#include "Core/Functions/Memory.h"
#include "Core/Services/StaticMemory.h" // SystemPersistent + the active-flag array sizing
#include "Core/Services/Script.h"

// Function declarations
void LoadAllBackups();

// Device identity (mandatory, see Core/Functions/Device.h).
extern const DeviceType KDeviceType = DeviceType::Tamu_v2_0A;
// Core (ID assignment, SN registry, time sync), the app link, and both user
// memory services - matching the USE_* build flags so the app shows their views.
// Subscriptions compiles both roles (USE_SUB_REQUEST + USE_SUB_PROVIDE).
// Node is advertised (Docs/Devices.md: "Tamu v2.0A ... Core and Node") but is decorative
// for now: Node is an end device (sensor/actuator/display), and no firmware behaviour reads
// the bit yet - it may matter for networking later.
extern const uint32_t KCapabilities = Capabilities::Core |
                                        Capabilities::Node |
                                        Capabilities::DynamicMemory |
                                        Capabilities::Scripts |
                                        Capabilities::AppInterface |
                                        Capabilities::SubscriptionRequest |
                                        Capabilities::SubscriptionProvide;

// Reads the factory MAC from eFuse as the 14-byte serial number (cached).
//
// The identity is the low 48 bits of that MAC (KSerialIdentityLen): the serial's first six bytes
// in printed order, e.g. E4:B0:63:C8:20:70 -> E4B063C82070, which is what the app shows and what
// the naming rule's tail (Blocks/AutoName.h: "Tamu-2070") is taken from.
const SerialNumber &GetSerialNumber()
{
    static SerialNumber Sn;
    static bool Init = false;
    if (!Init)
    {
        esp_efuse_read_field_blob(ESP_EFUSE_MAC_FACTORY, &Sn, 48);
        Init = true;
    }
    return Sn;
}


#include "Base.h"
#include "Blocks/Button.h"
#include "Blocks/PWM.h"
#include "Blocks/AccGyr.h"
#include "Blocks/BleSettings.h"
#ifdef USE_WIFI_SETTINGS
#include "Blocks/WifiSettings.h" // compile-in option, off in every shipped env (platformio.ini)
#endif
#include "Blocks/Vysi1Display.h"

// The static memory: two flat spaces (Docs/Services/Register.md "System + Static memory
// blocks"). Persistent settings (mirrored 1:1 to .SV) and volatile values, stacked in
// BlockInfo order (lowest BlockType first, instances contiguous per type). The block code
// addresses the fields through these, so the whole space is one compile-time layout.
struct StaticPersistent {
    SystemPersistent System;      // BlockType 0 (special)
    PWMPersistent Fan[2];         // BlockType 4
    AccGyrPersistent Accgyr;      // BlockType 5
    Vysi1Persistent Display[2];   // BlockType 6
    BleSettingsPersistent BleSet; // BlockType 0x0B: BLE enable/display name + the retired passkey bytes
#ifdef USE_WIFI_SETTINGS
    // BlockType 0x0C, and LAST on purpose: `.SV` is this whole space mirrored 1:1, so appending keeps
    // every other field's offset and a mirror written with the option on decodes as a valid prefix
    // without it (the WiFi fields then fall back to their compiled-in defaults).
    WifiSettingsPersistent WifiSet; // WiFi: enable + SSID (the password is in NVS, never here)
#endif
};
struct StaticVolatile {
    LEDButtonVolatile LedButton;  // BlockType 3
    PWMVolatile Fan[2];           // BlockType 4
    AccGyrVolatile Accgyr;        // BlockType 5
    Vysi1Volatile Display[2];     // BlockType 6
    // BlockType 0x0B: the passkey text, the mode, the bond list and the window countdown.
    BleSettingsVolatile BleSet;
#ifdef USE_WIFI_SETTINGS
    WifiSettingsVolatile WifiSet; // BlockType 0x0C: State/IP (read-only) + the always-empty password
#endif
};
// The System Name this build ships with: a fixed 16-char space-padded field with no terminator.
// It is the initialiser's only source, AND the naming rule's "factory value" (Blocks/AutoName.h):
// a field still holding it has never been given a name, so the first bond may fill it
// (Devices/Tamu_v2.0A/BleAutoName.h). The two must stay the same bytes - hence one constant.
constexpr SystemPersistent KFactorySystem = {
    {'T','a','m','u',' ','v','2','.','0','A',' ',' ',' ',' ',' ',' '},
#ifdef TYPE_CORE
    0, // NetId: unassigned until the boot picks one
#endif
};

StaticPersistent StaticPer = {
    .System = KFactorySystem,
    .Fan = {},
    .Accgyr = {},
    .Display = {},
    .BleSet = {},
#ifdef USE_WIFI_SETTINGS
    .WifiSet = {},
#endif
};

StaticVolatile StaticVol;

Vysi1Display Display1(StaticVol.Display[0], StaticPer.Display[0]);
Vysi1Display Display2(StaticVol.Display[1], StaticPer.Display[1]);

// Registry order is load-bearing: the app reconstructs each entry's registry index from
// its block type + per-type instance order, so keep the entries grouped by type (see
// FindStaticBlock in Core/Services/RegisterDefs.h).
const StaticBlockDescriptor StaticBlockRegistry[] = {
    {&StaticVol.LedButton, nullptr, &LEDButtonSchema, "LEDButton"},
    {&StaticVol.Fan[0], &StaticPer.Fan[0], &PWMSchema, "Fan1"},
    {&StaticVol.Fan[1], &StaticPer.Fan[1], &PWMSchema, "Fan2"},
    {&StaticVol.Accgyr, &StaticPer.Accgyr, &AccGyrSchema, "AccGyr"},
    {&StaticVol.Display[0], &StaticPer.Display[0], &Vysi1Schema, "LEDDisplay"},
    {&StaticVol.Display[1], &StaticPer.Display[1], &Vysi1Schema, "LEDDisplay2"},
    {&StaticVol.BleSet, &StaticPer.BleSet, &BleSettingsSchema, "BLE Settings"},
#ifdef USE_WIFI_SETTINGS
    // Compile-in option (platformio.ini): with the flag off this entry does not exist, so the block
    // is absent from the enumeration rather than present-but-empty, and StaticBlockNum does not grow.
    {&StaticVol.WifiSet, &StaticPer.WifiSet, &WifiSettingsSchema, "WiFi Settings"}
#endif
    };
const size_t StaticBlockNum = sizeof(StaticBlockRegistry) / sizeof(StaticBlockDescriptor);

// The Subscriptions instance the main loop calls. Pulled here (not at the top) because
// Subscriptions.h includes Register.h, whose split files name the board's StaticPer above.
#include "Core/Services/Subscriptions.h"

#include "PWM.h"
#include "Button.h"
#include "AccGyr.h"
#include "LED.h"

LEDDriver LED(3, 0); // both LED strips: pins 3 (Display1) and 0 (Display2), sent in parallel

#include "AppUSB.h"
#include "AppBLE.h"
#include "BleAutoName.h"    // the first bond's auto-name: the System/Name write and its `.SV` mirror
#include "BleSettingsGlue.h" // the BLE Settings block's write triggers and read-only refresh
#include "BleResetPin.h"     // the GPIO8 BLE reset: its input, its LED and its action (AppBLEStackReset)
#ifdef USE_WIFI_SETTINGS
#include "WifiSettingsGlue.h" // the WiFi Settings block's write triggers, NVS password, per-loop state
#endif
#include "RSBus.h"

#ifdef TYPE_CORE
// The Device service's asynchronous bootloader passthrough poll is called from the main loop
// below. Dispatcher.h also includes Device.h, but only after this file's loop is written, so
// pull it in here (after RSBus.h, whose RS485SendRaw/ReceiveRaw the passthrough uses).
#include "Core/Services/Device.h"
#endif



// Main FreeRTOS task: initialises hardware, broadcasts its serial number to find a short address, then runs the main control loop (bus, buttons, IMU, display).
void ApplicationTask(void *PvParameters)
{
PinModeOutput(LED_NOTIFICATION_PIN);
    PinLow(LED_NOTIFICATION_PIN);
    DeviceStatus.ShortAddress = 0;

if (!AppInterface.Connected) { ESP_LOGI("INIT","b1 storage"); }
    Storage.Init();
if (!AppInterface.Connected) { ESP_LOGI("INIT","b2 backups"); }
    LoadAllBackups(); // restores name + net-id from .SV too
    // The passkey lives in NVS, not in `.SV` (it is a secret and the Storage service streams
    // files out by name). Load it before the radio starts, and purge any key an older build left
    // in the persistent space.
    BlePasskeyInit();
#ifdef USE_WIFI_SETTINGS
    // Same rule, other secret: the WiFi password lives in NVS and the block reads it back empty, so
    // prime its presence flag after `.SV` has been recalled (Enabled/SSID) and before the loop.
    WifiSettingsInit();
#endif
Script.BootLoad();         // loads SCR_XXX scripts flagged load-on-boot (Docs/Services/Script.md)
    // Each panel's map comes from the FILE its Layout File Name register names - the same load a
    // register write runs, done once here because the boot recall restores the field but does not
    // re-run its write trigger (owner rule, 2026-11-03: "No layout should be pre-loaded in the
    // firmware"). Nothing compiled in is applied: a panel whose name is blank or whose file cannot
    // be loaded ends up with NO map (LedCount 0), renders nothing and sends 0 pixels, which is the
    // honest state of a board nobody has given a layout to. The line below names the file read and
    // the result, so a dark panel says why.
    {
        Vysi1Display *Panels[2] = {&Display1, &Display2};
        for (uint32_t Panel = 0; Panel < 2; Panel++)
        {
            Vysi1Display &Disp = *Panels[Panel];
            const Vysi1LayoutFile::Result Applied = ApplyLayoutFromStorage(Disp);
            if (AppInterface.Connected)
                continue;
            // The field is a fixed 8-char space-padded name with no terminator.
            char Name[9];
            memcpy(Name, Disp.Per.LayoutFile, 8);
            Name[8] = '\0';
            for (int i = 7; i >= 0 && (Name[i] == ' ' || Name[i] == '\0'); i--)
                Name[i] = '\0';
            ESP_LOGI("INIT", "layout %u '%s': %s, %u px", (unsigned)(Panel + 1), Name,
                     Vysi1LayoutFile::ResultName(Applied), (unsigned)Disp.LedCount);
        }
    }
if (!AppInterface.Connected) { ESP_LOGI("INIT","b3 appif"); }
    AppInterface.Init();

    // BLE app link (Nordic UART service); advertised under the persisted System Name
    // (Docs: "If possible, the device name is shown in BLE advertising"). The Name field is
    // a fixed 16-char space-padded field with no terminator, trimmed into a C string here.
    char BleName[SYSTEM_NAME_LEN + 1];
    SystemNameCString(BleName);
if (!AppInterface.Connected) { ESP_LOGI("INIT","b4 ble"); }
    AppBLEInit(BleName);

if (!AppInterface.Connected) { ESP_LOGI("INIT","b5 usb"); }
    AppUSBInit(); AppUSBStartTask();

    // Seed PRNG with hardware RNG for CSMA backoff randomisation.
    SeedRand(esp_random());

if (!AppInterface.Connected) { ESP_LOGI("INIT","b6 rs485"); }
    SetupRS485();
if (!AppInterface.Connected) { ESP_LOGI("INIT","b7 pwm"); }
    SetupFanPWM();
if (!AppInterface.Connected) { ESP_LOGI("INIT","b8 imu"); }
    InitLSM6DS3();
LED.Setup();

    PinHigh(LED_NOTIFICATION_PIN);
    PinModeInput(LED_NOTIFICATION_PIN);

    // Bring the LED button up on the value the block carries now (its volatile default after a
    // boot - the field is a trigger, not a persistent one) so the pin matches the field from the
    // start. This is the block's own bring-up, alongside the module setups above rather than a
    // generic re-application of the fields the recall restored.
    OnLEDStateChange(StaticBlockRegistry[0], 3, (const void *)&StaticVol.LedButton.LEDState, sizeof(bool));

    // The USB-less BLE reset: GPIO8 as an input with the pull-up, sampled from the main loop below.
    // Its first sample is the boot level, so a contact held across a reset is not read as a hold.
    BleResetInit();

    // Restored net-ID (LoadAllBackups), or a random one: 0 and 0x3F are not allowed.
    if (DeviceStatus.NetId == 0 || DeviceStatus.NetId >= 0x3F)
    {
        DeviceStatus.NetId = (uint8_t)(1 + (RawRand() % 61));
    }
    if (!AppInterface.Connected)
        ESP_LOGI("INIT", "core net ID = %u", (unsigned)DeviceStatus.NetId);

    // The Tamu is always the core: its address is NetID.1 (no discovery needed, no button
    // check). Node addresses are MakeId(NetId, shortID) so id_src/id_tgt comparisons and
    // routing are net-qualified.
    DeviceStatus.ShortAddress = MakeId(DeviceStatus.NetId, 1);

    // Core discover (docs, "Core functions"): broadcast to all cores (3F.1) from NetID.1. A
    // response carrying a MATCHING net within 500 ms means this net is claimed twice on the
    // bus -> normal boot is aborted (issue logged; the main loop blinks the error LED) while
    // the core stays reachable via the app link to change the net-ID. The net is settled before
    // anything is pushed onto it.
    PacketFrame Cd;
    PacketConstruct(&Cd, ADDR_ALL_CORES,
                    MakeService(ServiceType::Device, 0x10),
                    NextSystemTrid(ServiceType::Device),
                    FLAG_REQACK | FLAG_START | FLAG_STOP,
                    (const uint8_t *)&GetSerialNumber(), sizeof(SerialNumber));
    SendAndVerifyPacket(Cd);

    uint32_t CdEnd = TimeFromBoot() + 500; // docs: responses expected within 500 ms
    while ((int32_t)(TimeFromBoot() - CdEnd) < 0)
    {
        ProcessBus();
        Sleep(10);
    }
    if (CoreCollisionFlag() && !AppInterface.Connected)
        ESP_LOGE("CORE", "Net-ID %u collides with another core - normal boot aborted",
                 (unsigned)DeviceStatus.NetId);

    // Re-register the provider side of any restored requester subscriptions now that the net is
    // settled and we have our bus address (the provider table is non-persistent per the docs).
    Subscriptions.ReRegister();

    // The SNDB lists the net's devices only: local devices with net 0, other cores with
    // their NetID.1. The core does not store itself - the app models the connected core as
    // id 1 - and the Discover handler ignores a request carrying our own serial, so a stray
    // self-discover cannot mint a bogus device ID.

    // Boot announcement (code 0): an informational log, not an error.
    ReportLog(MakeLog(false, (uint16_t)ServiceType::Device, 0, 0), PRIORITY_LOG);

    static bool IdentifyPrev = false;
    // Uptime of the first loop pass, so "healthy" is measured from the app actually running.
    const int64_t LoopBootUs = esp_timer_get_time();
    while (1)
    {
        int64_t LoopStart = esp_timer_get_time();
        // Boot budget (Bootloader/BootDecision.h): the factory image gives this app a bounded
        // number of starts before it parks in update mode, and only a run that proves itself
        // refunds them. Three seconds is measured against the bring-up above, which finishes at
        // 0.5 s on the rig (BLE radio up, USB, storage recalled), so an image that starts and
        // dies - or reboot-loops - keeps spending its budget instead of refunding it on every
        // crash, while ordinary tooling that resets the board a few times still gets its refund.
        // Once per boot.
        static bool BootBudgetRefunded = false;
        if (!BootBudgetRefunded && LoopStart - LoopBootUs >= 3000000)
        {
            UpdaterBoot::StoreFailedBoots(UpdaterBoot::HealthyBoots());
            BootBudgetRefunded = true;
        }
        // Net-ID collision (Core-discover): blink the error LED slowly (~1 Hz) until
        // the net is changed. Takes priority over identify and the LED-button state.
        if (CoreCollisionFlag())
        {
            PinModeOutput(LED_NOTIFICATION_PIN);
            gpio_set_level(LED_NOTIFICATION_PIN, ((DeviceStatus.UptimeMs / 500) & 1) ? 1 : 0);
            IdentifyPrev = false; // keep the identify restore logic in sync
        }
        else
        {
        // Identify (Device CID 2): blink the notification LED fast (~5 Hz) while a
        // host asks us to identify ourselves, then re-apply the LED-button state.
        bool Ident = DeviceIdentifyActive(DeviceStatus.UptimeMs);
        if (Ident != IdentifyPrev)
        {
            if (Ident)
                PinModeOutput(LED_NOTIFICATION_PIN);
            else
                OnLEDStateChange(StaticBlockRegistry[0], 3, (const void *)&StaticVol.LedButton.LEDState, sizeof(bool));
            IdentifyPrev = Ident;
        }
        if (Ident)
            gpio_set_level(LED_NOTIFICATION_PIN, ((DeviceStatus.UptimeMs / 100) & 1) ? 1 : 0);
        }

        ProcessBus();
        Subscriptions.Tick(DeviceStatus.UptimeMs); // periodic provider triggers (docs: checked from the main loop)
        Script.Tick(DeviceStatus.UptimeMs);       // run loaded script programs (Docs/Services/Script.md)
#ifdef TYPE_CORE
        DeviceSvc.PollBootloaderPassthrough();    // settles a bootloader passthrough (CIDs 0x20-0x22)
#endif
        AppInterface.Pump();
        BleSettingsSync(); // the BLE Settings block's read-only fields follow the radio
#ifdef USE_WIFI_SETTINGS
        WifiSettingsSync(); // the WiFi Settings block's State/IP follow its configuration (no radio)
#endif
        ButtonUpdate();
        // The GPIO8 BLE reset: one sample of its input per pass, the LED it owns while a sequence
        // runs, and the stack restart / bond clear when a threshold is crossed. After ButtonUpdate so
        // the button's report and the sequencer's ownership are decided in the same pass.
        BleResetService();

        // Sample the IMU at its configured output data rate instead of every loop
        // (the sensor registers only change at ODR, so faster reads are wasted bus time).
        {
            // ODR periods (ms) for 12.5/26/52/104/208/416/833/1660 Hz. The loop below
            // sleeps 2 ms, so the achieved rate is floored at ~500 Hz: entries 6/7
            // (833/1660 Hz) clamp to 2 instead of the unreachable 1 ms.
            static const uint16_t OdrPeriodMs[8] = {80, 38, 19, 10, 5, 2, 2, 2};
            static uint32_t LastImuMs = 0;
            uint8_t Odr = StaticPer.Accgyr.SamplingRate < 8 ? StaticPer.Accgyr.SamplingRate : 3;
            if (DeviceStatus.UptimeMs - LastImuMs >= OdrPeriodMs[Odr]) {
                LastImuMs = DeviceStatus.UptimeMs;
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
        // The pixel count comes from each panel's own layout file (highest LED index it carries,
        // + 1), not from a firmware constant: a panel whose layout addresses LED 86 sends 87
        // pixels on the same image as one that addresses 86. Both strips share one bit timeline,
        // so the longer count is sent - a shorter chain passes the extra pixels out of its last
        // LED (a WS2812 latches the first N values it sees) and stays dark past its end.
        const uint16_t SendLen = Display1.LedCount > Display2.LedCount ? Display1.LedCount : Display2.LedCount;
        // No map, no frame: a panel whose layout file was not loaded (LedCount 0) is not driven at
        // all. A strip that was never latched - a fresh board, or one whose layout could not be
        // loaded - therefore stays dark instead of showing a frame built from nothing.
        if (SendLen > 0)
            LED.SendParallel(Display1.Buffer, Display2.Buffer, SendLen);
        {
            // FPS EMA weights (alpha = 0.1) in 16.16 fixed point; the pair sums to exactly
            // 1.0 so the average does not drift.
            static const Number FpsEmaNew = Number::FromRaw(6554);          // 0.1
            static const Number FpsEmaKeep = Number::FromRaw(65536 - 6554); // 0.9
            static int64_t LastFrameUs = 0;
            if (LastFrameUs != 0)
            {
                // FPS = 1e6 / period_us, kept in 16.16 fixed point (no float).
                int64_t PeriodUs = LoopStart - LastFrameUs;
                if (PeriodUs <= 0) PeriodUs = 1;
                Number Inst = Number::FromRaw((int32_t)((1000000LL << 16) / PeriodUs));
                Display1.Vol.RefreshRate = Display1.Vol.RefreshRate * FpsEmaKeep + Inst * FpsEmaNew;
                Display2.Vol.RefreshRate = Display2.Vol.RefreshRate * FpsEmaKeep + Inst * FpsEmaNew;
            }
            LastFrameUs = LoopStart;
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
        const esp_partition_t *Factory = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
        if (Factory != nullptr) {
            // A failed re-arm leaves the next reset booting THIS app instead of the bootloader,
            // so the button-catch path is unreachable and an update can strand the device. Report
            // it rather than discarding the esp_err_t.
            esp_err_t ArmErr = esp_ota_set_boot_partition(Factory);
            if (ArmErr != ESP_OK)
                ESP_LOGE("BOOT", "factory boot re-arm failed: %d", (int)ArmErr);
        }

        // 16 KB: LoadAllBackups places a MEMORY_BACKUP_CAP-sized buffer on this stack and
        // ProcessBus dispatches full request/response chains recursively beneath it.
        xTaskCreate(ApplicationTask, "app_task", 16384, NULL, 5, NULL);
    };
}
