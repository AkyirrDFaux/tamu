#pragma once

#include "ch32v20x.h"

// Forward declarations for the core routines the device loop calls (defined in the shared
// Core, included later in the translation unit).
void LoadAllBackups();
void ScriptsTick(uint32_t nowMs);

#include "Base.h"
#include "Bus.h"
#include "Log.h"
#include "Storage.h"
#include "Measuring.h"
#include "Blocks/Button.h"
#include "Blocks/PWM.h"
#include "Blocks/Vysi1Display.h"
#include "Core/Functions/Device.h"
#include "Core/Services/StaticMemory.h"

// CH32V20x electronic signature area (same address the SDK's own FLASH_GetMACAddress reads):
// the 96-bit unique device ID lives at 0x1FFFF7E8. Used as the 14-byte serial number.
#define CHIP_ID_ADDR 0x1FFFF7E8

// Device identity (mandatory, see Core/Functions/Device.h).
extern const DeviceType kDeviceType = DeviceType::Valu_v2_0;
// Capabilities per Docs/Services/System Block and Device Commands.md, matched to the build
// flags and Docs/Devices.md's service list:
//   Node                 - a standalone node (not a core: no SNDB, no core discovery)
//   StorageFiles         - the full file create/delete/rename/resize service (Mandatory
//                          Storage; the Valu has an 8 kB filesystem, like the DAS)
//   AppInterface         - the USB CDC App Interface (USE_APP_INTERFACE)
//   DynamicMemory        - the optional Dynamic memory service (USE_DYNAMIC_BLOCKS)
//   Scripts              - the optional Script service (USE_SCRIPTS)
// NOTE: the docs also list a "USB Bootloader" service for this board, but the capability
// field has no bit for it (Core/Types/Enums.h) - reported, not invented. The mandatory
// System/Register and Log services are always compiled and carry no capability bit.
extern const uint32_t kCapabilities =
    Capabilities::Node | Capabilities::StorageFiles | Capabilities::AppInterface |
    Capabilities::DynamicMemory | Capabilities::Scripts;

// Reads the CH32V203 96-bit unique ID as the 14-byte serial number (cached, zero-padded).
const SerialNumber &GetSerialNumber()
{
    static SerialNumber sn;
    static bool init = false;
    if (!init)
    {
        memset(sn.bytes, 0, sizeof(sn.bytes));
        memcpy(sn.bytes, (const void *)CHIP_ID_ADDR, 12);
        init = true;
    }
    return sn;
}

// The static memory: two flat spaces (Docs/Services/Register.md "System + Static memory
// blocks"). Persistent settings (mirrored 1:1 to .SV) and volatile values, stacked in
// BlockInfo order (lowest BlockType first, instances contiguous per type).
struct StaticPersistent {
    SystemPersistent system;          // BlockType 0 (special)
    PWMPersistent fan[1];             // BlockType 4
    Vysi1Persistent display[2];       // BlockType 6
    ResistiveMeasPersistent meas[3];  // BlockType 8
};
struct StaticVolatile {
    LEDButtonVolatile ledButton;      // BlockType 3
    ButtonVolatile button[3];         // BlockType 9
    PWMVolatile fan[1];               // BlockType 4
    Vysi1Volatile display[2];         // BlockType 6
    ResistiveMeasVolatile meas[3];    // BlockType 8
};
StaticPersistent staticPer = {
    // Name is a fixed 16-char space-padded field (no terminator).
    .system = {.Name = {'V','a','l','u',' ','v','2','.','0',' ',' ',' ',' ',' ',' ',' '}},
    .fan = {},
    .display = {},
    .meas = {},
};
StaticVolatile staticVol;

// The two LED display instances (Docs/Devices.md "Valu v2.0": "LED Display, 2 instances").
// The Vysi1 display block is generic - it also drives bare LED strips, with the strip
// geometry coming from the layout file at runtime - so registering the two instances is
// bookkeeping rather than a fixed pin mapping. Constructing them self-registers each into
// Vysi1Display::s_instances, which the block's LayoutFile write trigger resolves the owner
// from. No render/driver loop runs on this board yet (it has no WS2812 strip driver), so
// the blocks are enumerable and writable but produce no light.
Vysi1Display Display1(staticVol.display[0], staticPer.display[0]);
Vysi1Display Display2(staticVol.display[1], staticPer.display[1]);

// Registry order is load-bearing: the app reconstructs each entry's registry index from
// its block type + per-type instance order, so keep the entries grouped by type, ascending
// (LEDButton 0x03, PWM 0x04, Vysi1Display 0x06, ResistiveMeasure 0x08, Button 0x09).
const StaticBlockDescriptor static_block_registry[] = {
    {&staticVol.ledButton, nullptr, &LEDButton_Schema, "LEDButton"},
    {&staticVol.fan[0], &staticPer.fan[0], &PWM_Schema, "Fan1"},
    {&staticVol.display[0], &staticPer.display[0], &Vysi1_Schema, "LEDDisplay"},
    {&staticVol.display[1], &staticPer.display[1], &Vysi1_Schema, "LEDDisplay2"},
    {&staticVol.meas[0], &staticPer.meas[0], &ResistiveMeas_Schema, "Meas1"},
    {&staticVol.meas[1], &staticPer.meas[1], &ResistiveMeas_Schema, "Meas2"},
    {&staticVol.meas[2], &staticPer.meas[2], &ResistiveMeas_Schema, "Meas3"},
    {&staticVol.button[0], nullptr, &Button_Schema, "Button1"},
    {&staticVol.button[1], nullptr, &Button_Schema, "Button2"},
    {&staticVol.button[2], nullptr, &Button_Schema, "Button3"},
};
const size_t static_block_num = sizeof(static_block_registry) / sizeof(StaticBlockDescriptor);

// The registry index of the LED-Button (the LED state lives on field 3); used to re-apply the
// restored LED state after boot.
#define LEDBUTTON_REG_INDEX 0

// Device implementations (drive the actual pins); included after the block instances.
#include "PWM.h"
#include "Button.h"
#include "LED.h"
#include "MeasuringRun.h"

// USB App Interface (brings up TinyUSB and implements the AppInterface link hooks).
#include "AppUSB.h"

// SystemInit wrapper: the bootloader has already set the clock; re-running the framework's
// SystemInit hangs (see System.h). Must be included in the single translation unit.
#include "System.h"

// Device entry point: brings up the peripherals, restores persisted state, starts the USB
// App Interface, then runs the main control loop (buttons, measurement, LED, USB link).
int main(void)
{
    // System init. The bootloader jumps here with interrupts disabled; enable the SysTick
    // free-running counter (CTLR: ENABLE | CLKSOURCE = HCLK, CMP = 0) for TimeFromBoot().
    SystemCoreClockUpdate();
    SysTick->CTLR |= 0x05;

    // GPIOA carries the LED-Button (PA2) and the fan PWM (PA8); GPIOB (buttons) is enabled in
    // ButtonsInit().
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    // LED-Button idle: LED off -> input with pull-down (the line is then readable).
    PinModeInputPullDown(VALU_LED_PORT, VALU_LED_PIN);

    DeviceStatus.ShortAddress = 0;

    // Seed the PRNG (CSMA backoff / random gaps) with the live SysTick counter.
    SeedRand(SysTick->CNT);

    // Persistent storage: formats the flash directory if needed and restores the saved system
    // state (.SV).
    Storage.Init();
    LoadAllBackups();

    // LED displays: preload the default layout file (LAY_1) if absent, then apply the
    // (restored) LayoutFile to each display. The boot recall restores the LayoutFile RAM
    // field but does not re-run its write trigger, so the layout must be applied explicitly
    // (mirrors the core's boot sequence).
    PreloadVysiLayout();
    Vysi1BootLayout(Display1);
    Vysi1BootLayout(Display2);

    ButtonsInit();
    Measuring_Init();
    SetupFanPWM();

    // USB App Interface: allocate the TX ring (AppInterfaceInit) then bring the controller up.
    AppInterfaceInit();
    AppUSBInit();

    // Standalone device: this board has no RSBus (Docs/Devices.md lists no bus/Router service),
    // so there is no discovery exchange. The app reaches it by port and addresses it as the
    // local net's device 1; net 0 targets resolve to NetId (NetQualifyLocal), so device 1 on
    // net 0 lands here.
    DeviceStatus.NetId = 1;
    DeviceStatus.ShortAddress = MakeId(DeviceStatus.NetId, 1);

    // Re-apply the restored LED state to the pin (the boot restore writes the RAM field but
    // does not re-run its write trigger).
    OnLEDStateChange(static_block_registry[LEDBUTTON_REG_INDEX], 3,
                     (const void *)&staticVol.ledButton.LEDState, sizeof(bool));

    uint32_t last_sample_ms[MEAS_CHANNEL_COUNT] = {0, 0, 0};
    static bool s_ident_prev = false;

    while (1)
    {
        TimeUpdate();
        // USB App Interface: pump TinyUSB, dispatch queued inbound app frames, flush replies.
        AppInterfacePump();

        ButtonsUpdate();
        LEDButtonUpdate();
        ScriptsTick(DeviceStatus.UptimeMs); // run loaded script programs (Docs/Services/Script.md)

        uint32_t now_ms = Now();

        // Sample each resistive channel at its own configured rate.
        for (uint8_t c = 0; c < MEAS_CHANNEL_COUNT; c++)
        {
            if ((now_ms - last_sample_ms[c]) >= SampleIntervalMs(c))
            {
                last_sample_ms[c] = now_ms;
                Measuring_Update(c, Meas_AdcRead(s_meas_adc_ch[c]));
            }
        }

        // Red LED (LED-Button PA2), priority overlays like the other devices: an active
        // identify blink overrides the block's LEDState; otherwise the block value drives it.
        bool ident = DeviceIdentifyActive(now_ms);
        if (ident != s_ident_prev)
        {
            if (!ident)
                OnLEDStateChange(static_block_registry[LEDBUTTON_REG_INDEX], 3,
                                 (const void *)&staticVol.ledButton.LEDState, sizeof(bool));
            s_ident_prev = ident;
        }
        if (ident)
        {
            PinModeOutput(VALU_LED_PORT, VALU_LED_PIN);
            if (((now_ms / 100) & 1) == 0) PinHigh(VALU_LED_PORT, VALU_LED_PIN);
            else PinLow(VALU_LED_PORT, VALU_LED_PIN);
        }

        Sleep(5); // short heartbeat; the USB link is polled by AppInterfacePump each iteration
    }
}
