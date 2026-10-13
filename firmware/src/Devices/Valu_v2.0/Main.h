#pragma once

#include "ch32v20x.h"

// Forward declarations for the core routines the device loop calls (defined in the shared
// Core, included later in the translation unit).
void LoadAllBackups();

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
#include "Core/Services/Script.h"

// CH32V20x electronic signature area (same address the SDK's own FLASH_GetMACAddress reads):
// the 96-bit unique device ID lives at 0x1FFFF7E8. Used as the 14-byte serial number.
#define CHIP_ID_ADDR 0x1FFFF7E8

// Device identity (mandatory, see Core/Functions/Device.h).
extern const DeviceType KDeviceType = DeviceType::Valu_v2_0;
// Capabilities per Docs/Services/System Block and Device Commands.md, matched to the build
// flags and Docs/Devices.md's service list:
//   Node                 - a standalone node (not a core: no SNDB, no core discovery)
//   AppInterface         - the USB CDC App Interface (USE_APP_INTERFACE)
//   DynamicMemory        - the optional Dynamic memory service (USE_DYNAMIC_BLOCKS)
//   Scripts              - the optional Script service (USE_SCRIPTS)
// NOTE: the docs also list a "USB Bootloader" service for this board, but the capability
// field has no bit for it (Core/Types/Enums.h) - reported, not invented. The mandatory
// System/Register and Log services are always compiled and carry no capability bit.
extern const uint32_t KCapabilities =
    Capabilities::Node | Capabilities::AppInterface |
    Capabilities::DynamicMemory | Capabilities::Scripts;

// Reads the CH32V203 96-bit unique ID as the 14-byte serial number (cached, zero-padded).
const SerialNumber &GetSerialNumber()
{
    static SerialNumber Sn;
    static bool Init = false;
    if (!Init)
    {
        memset(Sn.Bytes, 0, sizeof(Sn.Bytes));
        memcpy(Sn.Bytes, (const void *)CHIP_ID_ADDR, 12);
        Init = true;
    }
    return Sn;
}

// The static memory: two flat spaces (Docs/Services/Register.md "System + Static memory
// blocks"). Persistent settings (mirrored 1:1 to .SV) and volatile values, stacked in
// BlockInfo order (lowest BlockType first, instances contiguous per type).
struct StaticPersistent {
    SystemPersistent System;          // BlockType 0 (special)
    PWMPersistent Fan[1];             // BlockType 4
    Vysi1Persistent Display[2];       // BlockType 6
    ResistiveMeasPersistent Meas[3];  // BlockType 8
};
struct StaticVolatile {
    LEDButtonVolatile LedButton;      // BlockType 3
    ButtonVolatile Button[3];         // BlockType 9
    PWMVolatile Fan[1];               // BlockType 4
    Vysi1Volatile Display[2];         // BlockType 6
    ResistiveMeasVolatile Meas[3];    // BlockType 8
};
StaticPersistent StaticPer = {
    // Name is a fixed 16-char null-padded field.
    .System = {.Name = {'V','a','l','u',' ','v','2','.','0',' ',' ',' ',' ',' ',' ',' '}},
    .Fan = {},
    .Display = {},
    .Meas = {},
};
StaticVolatile StaticVol;

// The two LED display instances (Docs/Devices.md "Valu v2.0": "LED Display, 2 instances").
// The Vysi1 display block is generic - it also drives bare LED strips, with the strip
// geometry coming from the layout file at runtime - so registering the two instances is
// bookkeeping rather than a fixed pin mapping. Constructing them self-registers each into
// Vysi1Display::s_instances, which the block's LayoutFile write trigger resolves the owner
// from. No render/driver loop runs on this board yet (it has no WS2812 strip driver), so
// the blocks are enumerable and writable but produce no light.
Vysi1Display Display1(StaticVol.Display[0], StaticPer.Display[0]);
Vysi1Display Display2(StaticVol.Display[1], StaticPer.Display[1]);

// Registry order is load-bearing: the app reconstructs each entry's registry index from
// its block type + per-type instance order, so keep the entries grouped by type, ascending
// (LEDButton 0x03, PWM 0x04, Vysi1Display 0x06, ResistiveMeasure 0x08, Button 0x09).
const StaticBlockDescriptor StaticBlockRegistry[] = {
    {&StaticVol.LedButton, nullptr, &LEDButtonSchema, "LEDButton"},
    {&StaticVol.Fan[0], &StaticPer.Fan[0], &PWMSchema, "Fan1"},
    {&StaticVol.Display[0], &StaticPer.Display[0], &Vysi1Schema, "LEDDisplay"},
    {&StaticVol.Display[1], &StaticPer.Display[1], &Vysi1Schema, "LEDDisplay2"},
    {&StaticVol.Meas[0], &StaticPer.Meas[0], &ResistiveMeasSchema, "Meas1"},
    {&StaticVol.Meas[1], &StaticPer.Meas[1], &ResistiveMeasSchema, "Meas2"},
    {&StaticVol.Meas[2], &StaticPer.Meas[2], &ResistiveMeasSchema, "Meas3"},
    {&StaticVol.Button[0], nullptr, &ButtonSchema, "Button1"},
    {&StaticVol.Button[1], nullptr, &ButtonSchema, "Button2"},
    {&StaticVol.Button[2], nullptr, &ButtonSchema, "Button3"},
};
const size_t StaticBlockNum = sizeof(StaticBlockRegistry) / sizeof(StaticBlockDescriptor);

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
    MeasuringInit();
    SetupFanPWM();

    // USB App Interface: allocate the TX ring (AppInterfaceInit) then bring the controller up.
    AppInterface.Init();
    AppUSBInit();

    // Standalone device: this board has no RSBus (Docs/Devices.md lists no bus/Router service),
    // so there is no discovery exchange. The app reaches it by port and addresses it as the
    // local net's device 1; net 0 targets resolve to NetId (NetQualifyLocal), so device 1 on
    // net 0 lands here.
    DeviceStatus.NetId = 1;
    DeviceStatus.ShortAddress = MakeId(DeviceStatus.NetId, 1);

    // Re-apply the restored LED state to the pin (the boot restore writes the RAM field but
    // does not re-run its write trigger).
    OnLEDStateChange(StaticBlockRegistry[LEDBUTTON_REG_INDEX], 3,
                     (const void *)&StaticVol.LedButton.LEDState, sizeof(bool));

    uint32_t LastSampleMs[MEAS_CHANNEL_COUNT] = {0, 0, 0};
    static bool IdentPrev = false;

    while (1)
    {
        TimeUpdate();
        // USB App Interface: pump TinyUSB, dispatch queued inbound app frames, flush replies.
        AppInterface.Pump();

        ButtonsUpdate();
        LEDButtonUpdate();
        Script.Tick(DeviceStatus.UptimeMs); // run loaded script programs (Docs/Services/Script.md)

        uint32_t NowMs = Now();

        // Sample each resistive channel at its own configured rate.
        for (uint8_t Channel = 0; Channel < MEAS_CHANNEL_COUNT; Channel++)
        {
            if ((NowMs - LastSampleMs[Channel]) >= SampleIntervalMs(Channel))
            {
                LastSampleMs[Channel] = NowMs;
                MeasuringUpdate(Channel, MeasAdcRead(MeasAdcCh[Channel]));
            }
        }

        // Red LED (LED-Button PA2), priority overlays like the other devices: an active
        // identify blink overrides the block's LEDState; otherwise the block value drives it.
        bool Ident = DeviceIdentifyActive(NowMs);
        if (Ident != IdentPrev)
        {
            if (!Ident)
                OnLEDStateChange(StaticBlockRegistry[LEDBUTTON_REG_INDEX], 3,
                                 (const void *)&StaticVol.LedButton.LEDState, sizeof(bool));
            IdentPrev = Ident;
        }
        if (Ident)
        {
            PinModeOutput(VALU_LED_PORT, VALU_LED_PIN);
            if (((NowMs / 100) & 1) == 0) PinHigh(VALU_LED_PORT, VALU_LED_PIN);
            else PinLow(VALU_LED_PORT, VALU_LED_PIN);
        }

        Sleep(5); // short heartbeat; the USB link is polled by AppInterface.Pump() each iteration
    }
}
