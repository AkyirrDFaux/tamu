#pragma once

#include "ch32v00x.h"
#include "debug.h"

// NOTE: DeviceLog is compiled out via a no-op macro in src/Main.cpp
// (DEVICE_LOG_TEXTLESS) - the DAS log format carries no free text, see Log.h.

// Forward declaration for LoadAllBackups function
void LoadAllBackups();

#include "Base.h"
#include "RSBus.h"
#include "Log.h"
#include "Storage.h"
#include "Measuring.h"
#include "Blocks/Button.h"
#include "Blocks/LED.h"
#include "Core/Functions/Device.h"
#include "Core/Services/StaticMemory.h"

#define CHIP_ID_ADDR  0x1FFFF7E8 // Fixed memory-mapped location of the chip's unique ID

// Node time-sync interval. The docs say "repeated at random within the next 2-3 minutes";
// the 60-75 s cadence (after the 30 s warm-up) keeps the holdover within the required
// <10 ms. The DAS's internal RC oscillator drifts ~1% (see Core/Functions/SysFunctions.h);
// the two-point drift estimate cancels that first-order term, leaving the residual change in
// the drift rate (~0.02% across the interval, i.e. ~10 ms per 60 s).
#define NODE_TIME_SYNC_INTERVAL_MS 60000u
// First re-sync delay: a short warm-up so the drift estimate is seeded early.
#define NODE_TIME_SYNC_WARMUP_MS 30000u

// Node discovery repeat: while unregistered the node broadcasts Device CID 0 every ~500 ms,
// jittered +/-25% so nodes that power up together do not synchronise their discover bursts
// (Docs/Services/System Block and Device Commands.md: "at random intervals until their ID is
// assigned"). Nominal unchanged; RawRand() is the shared codebase PRNG.
#define NODE_DISCOVER_INTERVAL_MS 500u
#define NODE_DISCOVER_JITTER_MS   125u // +/-25%

// Device identity (mandatory, see Core/Functions/Device.h).
extern const DeviceType KDeviceType = DeviceType::DualAnalogSensor;
// The DAS runs the full multi-file filesystem (create/delete/rename/resize); storage files
// are universal so they carry no capability bit (Core/Types/Enums.h).
extern const uint32_t KCapabilities =
    Capabilities::Node | Capabilities::SubscriptionProvide;

// Reads the CH32V003 32-bit unique chip ID as the 14-byte serial number (cached).
const SerialNumber &GetSerialNumber()
{
    static SerialNumber Sn;
    static bool Init = false;
    if (!Init)
    {
        uint32_t ChipId = *(uint32_t *)(CHIP_ID_ADDR);
        memset(Sn.Bytes, 0, 14);
        memcpy(Sn.Bytes, &ChipId, 4);
        Init = true;
    }
    return Sn;
}

// The static memory: two flat spaces (Docs/Services/Register.md "System + Static memory
// blocks"). Persistent settings (mirrored 1:1 to .SV) and volatile values, stacked in
// BlockInfo order (lowest BlockType first, instances contiguous per type).
struct StaticPersistent {
    SystemPersistent System;          // BlockType 0 (special)
    ResistiveMeasPersistent Meas[2];  // BlockType 8
};
struct StaticVolatile {
    ResistiveMeasVolatile Meas[2];    // BlockType 8
    ButtonVolatile Button;            // BlockType 9
    LEDVolatile Led;                  // BlockType 0x0A
};
StaticPersistent StaticPer = {
    // Name is a fixed 16-char null-padded field.
    .System = {.Name = {'D','A','S',' ','v','0','.','1',' ',' ',' ',' ',' ',' ',' ',' '}},
    .Meas = {{.SensorType = MeasNTC100K}, {.SensorType = MeasLDR10K}},
};
StaticVolatile StaticVol;

// Registry order is load-bearing: the app derives the registry index from type + per-type
// instance order - keep the entries grouped by type.
const StaticBlockDescriptor StaticBlockRegistry[] = {
    {&StaticVol.Meas[0], &StaticPer.Meas[0], &ResistiveMeasSchema, "Meas1"},
    {&StaticVol.Meas[1], &StaticPer.Meas[1], &ResistiveMeasSchema, "Meas2"},
    {&StaticVol.Button, nullptr, &ButtonSchema, "Button"},
    {&StaticVol.Led, nullptr, &LEDSchema, "LED"},
};
const size_t StaticBlockNum = sizeof(StaticBlockRegistry) / sizeof(StaticBlockDescriptor);

// The Subscriptions instance the main loop calls, pulled after the board's StaticPer above.
#include "Core/Services/Subscriptions.h"

// DAS device implementations (drive the actual pins); included after the block instances.
#include "Button.h"
#include "LED.h"
#include "MeasuringRun.h"

// Converts a configured sampling rate (Hz) into the loop interval in ms (>= 1 ms).
// Memoised per channel: the 32-bit software divide runs only when the stored rate changes,
// so polling both channels every loop costs a compare instead of two divides. The cache is
// keyed on the rate itself (not just seeded by the Sampling Rate write trigger) so a Recall
// that writes the persistent space directly also refreshes it.
static inline uint32_t SampleIntervalMs(uint8_t Channel)
{
    static uint32_t Cache[2] = {0, 0}; // (hz << 16) | interval_ms
    int32_t Hz = StaticPer.Meas[Channel].SamplingRate.Value >> 16; // integer Hz
    if (Hz <= 0) Hz = 10;     // disabled / invalid -> default 10 Hz
    if (Hz > 1000) Hz = 1000; // matches the Sampling Rate write clamp
    if ((Cache[Channel] >> 16) != (uint32_t)Hz)
    {
        uint32_t Interval = 1000u / (uint32_t)Hz;
        if (Interval == 0) Interval = 1;
        Cache[Channel] = ((uint32_t)Hz << 16) | Interval;
    }
    return Cache[Channel] & 0xFFFFu;
}

// Device entry point: initialises hardware, discovers its short address over RS485, then blinks the status LED.
int main(void)
{
    // Initialize system
    SystemCoreClockUpdate();
    Delay_Init();
    SysTick->CTLR |= 0x05;

    // Enable clocks for GPIO Port A and Port D
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOD, ENABLE);

    PinModeOutput(LEDR);
    PinModeOutput(LEDW);
    PinLow(LEDW);
    PinHigh(LEDR);

    DeviceStatus.ShortAddress = 0;

    // Seed PRNG with SysTick counter (unique per power-on) for CSMA backoff randomisation.
    SeedRand(SysTick->CNT);

    SetupRS485();
    MeasuringInit();
    DasButtonInit();

    // Persistent storage: formats the flash directory if needed and restores the saved
    // system state. The short address is cleared afterwards so a changed network always
    // gets a fresh registration (SNumber/name persistence still works).
    Storage.Init();
    LoadAllBackups();
    DeviceStatus.ShortAddress = 0;

    // Core distinction loop
    while (DeviceStatus.ShortAddress == 0)
    {
        PacketConstruct(&TxFrame, ADDR_BROADCAST,
                         MakeService(ServiceType::Device, 0),
                         NextSystemTrid(ServiceType::Device),
                         FLAG_REQACK | FLAG_START | FLAG_STOP,
                         (const uint8_t *)&GetSerialNumber(), sizeof(SerialNumber));
        DispatchPacket(TxFrame);
        // Jittered repeat: uniform over [375, 625] ms (+/-25% around 500 ms).
        Sleep(NODE_DISCOVER_INTERVAL_MS - NODE_DISCOVER_JITTER_MS
              + (RawRand() % (2 * NODE_DISCOVER_JITTER_MS + 1)));
        ProcessBus();
    }
    PinLow(LEDR);

    // TimeSync is synchronized-device initiated: this node syncs ITSELF to the core by
    // sending Device service CID 3 and computing the offset locally from the reply. Repeat
    // every 2-3 minutes, jittered so devices do not all burst at once
    // (Docs/Services/System Block and Device Commands.md).
    auto SendTimeSync = []() {
        // NTP-like: send the SYNCHRONIZED local time (Now()), so the reply's offset is a
        // correction (delta) to add to TimeOffsetMs, not an absolute value to overwrite.
        // TimeSync goes to the core of our net: our assigned address is NetID.shortID, so
        // the core is the same net with device ID 1.
        uint32_t TimeSent = Now();
        uint16_t CoreAddr = MakeId((uint8_t)((DeviceStatus.ShortAddress >> 10) & 0x3F), 1);
        PacketConstruct(&TxFrame, CoreAddr,
                         MakeService(ServiceType::Device, 3),
                         NextSystemTrid(ServiceType::Device),
                         FLAG_REQACK | FLAG_START | FLAG_STOP,
                         (const uint8_t *)&TimeSent, sizeof(uint32_t),
                         PRIORITY_TIMESYNC);
        DispatchPacket(TxFrame);
        // ProcessBus() in the main loop handles the reply (applies the offset).
    };
    SendTimeSync(); // initial sync right after discovery
    // The re-sync interval is measured in RAW time (TimeFromBoot): a clock correction
    // changes DeviceStatus.UptimeMs, and measuring against it would let the correction
    // itself satisfy the interval (a TimeSync storm).
    uint32_t LastSyncMs = TimeFromBoot();
    // Warm-up first (seed the drift estimate), then the documented 2-3 minute cadence.
    uint32_t NextSyncMs = NODE_TIME_SYNC_WARMUP_MS;

    uint32_t LastSampleMs[2] = {0, 0};
    static const uint8_t MeasAdcCh[2] = {MEAS1_ADC_CH, MEAS2_ADC_CH};

    while (1)
    {
        TimeUpdate();
        ProcessBus();
        Subscriptions.Tick(DeviceStatus.UptimeMs); // periodic provider triggers (main loop)

        // Re-sync to the core every ~60-75 s (synchronized-device initiated, jittered so
        // nodes do not burst together). Measured in raw time so a clock correction cannot
        // trigger the next sync.
        if ((TimeFromBoot() - LastSyncMs) >= NextSyncMs) {
            LastSyncMs = TimeFromBoot();
            NextSyncMs = NODE_TIME_SYNC_INTERVAL_MS + (RawRand() % 15000);
            SendTimeSync();
        }

        DasButtonUpdate(); // Button block edge detection/counter

        // Sample each resistive measurement channel at its own configured rate.
        uint32_t NowMs = Now();
        for (uint8_t Channel = 0; Channel < 2; Channel++)
        {
            if ((NowMs - LastSampleMs[Channel]) >= SampleIntervalMs(Channel))
            {
                LastSampleMs[Channel] = NowMs;
                MeasuringUpdate(Channel, MeasAdcRead(MeasAdcCh[Channel]));
            }
        }

        // Red LED with priority overlays: an active bus error blinks it (~2 Hz), else the
        // identify blink (~10 Hz), else the LED block's LEDState field. The white LED is
        // the RS485 TX activity indicator (driven inside RSBus.h).
        bool RedState;
        if (DasErrorFlag)
            RedState = ((NowMs / 500) & 1) == 0;
        else if (DeviceIdentifyActive(NowMs))
            RedState = ((NowMs / 100) & 1) == 0;
        else
            RedState = StaticVol.Led.LEDState;
        if (RedState) PinHigh(LEDR); else PinLow(LEDR);
    }
}
