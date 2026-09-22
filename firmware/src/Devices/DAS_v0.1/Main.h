#pragma once

#include "ch32v00x.h"
#include "debug.h"

// NOTE: DeviceLog is compiled out via a no-op macro in src/Main.cpp
// (DEVICE_LOG_TEXTLESS) - the DAS log format carries no free text, see Log.h.

// Forward declaration for LoadAllBackups function
void LoadAllBackups();
void SubscriptionsTick(uint32_t nowMs);

// DAS has no app interface
bool AppConnected = false;

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

// Node time-sync interval (docs: "repeated at random within the next 2-3 minutes").
#define NODE_TIME_SYNC_INTERVAL_MS 150000u

// Device identity (mandatory, see Core/Functions/Device.h).
extern const DeviceType kDeviceType = DeviceType::DualAnalogSensor;
extern const uint32_t kCapabilities = Capabilities::Node | Capabilities::Subscriptions;

// Reads the CH32V003 32-bit unique chip ID as the 14-byte serial number (cached).
const SerialNumber &GetSerialNumber()
{
    static SerialNumber sn;
    static bool init = false;
    if (!init)
    {
        uint32_t chip_id = *(uint32_t *)(CHIP_ID_ADDR);
        memset(sn.bytes, 0, 14);
        memcpy(sn.bytes, &chip_id, 4);
        init = true;
    }
    return sn;
}

ResistiveMeasStruct Meas1(MeasNTC100K); // channel 1: 100k NTC thermistor
ResistiveMeasStruct Meas2(MeasLDR10K);  // channel 2: 10k LDR
ButtonStruct DasButton;
LEDStruct DasLed;

const StaticBlockDescriptor static_block_registry[] = {
    {&Meas1, &ResistiveMeas_Schema, "Meas1"},
    {&Meas2, &ResistiveMeas_Schema, "Meas2"},
    {&DasButton, &Button_Schema, "Button"},
    {&DasLed, &LED_Schema, "LED"},
};
const size_t static_block_num = sizeof(static_block_registry) / sizeof(StaticBlockDescriptor);

// DAS device implementations (drive the actual pins); included after the block instances.
#include "Button.h"
#include "LED.h"

// Converts a configured sampling rate (Hz) into the loop interval in ms (>= 1 ms).
static inline uint32_t SampleIntervalMs(Number rate)
{
    int32_t hz = rate.Value >> 16; // integer Hz
    if (hz <= 0) return 100;       // disabled / invalid -> default 10 Hz
    uint32_t interval = 1000u / (uint32_t)hz;
    return interval == 0 ? 1 : interval;
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
    Measuring_Init();
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
        PacketConstruct(&tx_frame, ADDR_BROADCAST,
                         MakeService(ServiceType::Device, 0),
                         MakeService(ServiceType::Device, 0),
                         FLAG_REQACK | FLAG_START | FLAG_STOP,
                         (const uint8_t *)&GetSerialNumber(), sizeof(SerialNumber));
        DispatchPacket(tx_frame);
        Sleep(500);
        ProcessBus();
    }
    PinLow(LEDR);

    // TimeSync is synchronized-device initiated: this node syncs ITSELF to the core by
    // sending Device service CID 3 and computing the offset locally from the reply. Repeat
    // every 2-3 minutes, jittered so devices do not all burst at once
    // (Docs/Services/System Block and Device Commands.md).
    auto sendTimeSync = []() {
        uint32_t time_sent = TimeFromBoot();
        PacketConstruct(&tx_frame, 1,
                         MakeService(ServiceType::Device, 3),
                         MakeService(ServiceType::Device, 3),
                         FLAG_REQACK | FLAG_START | FLAG_STOP,
                         (const uint8_t *)&time_sent, sizeof(uint32_t));
        DispatchPacket(tx_frame);
        // ProcessBus() in the main loop handles the reply (applies the offset).
    };
    sendTimeSync(); // initial sync right after discovery
    uint32_t last_sync_ms = DeviceStatus.UptimeMs;
    uint32_t next_sync_ms = NODE_TIME_SYNC_INTERVAL_MS + (RawRand() % 60000);

    uint32_t last_sample_ms = 0;
    uint32_t last_sample2_ms = 0;

    while (1)
    {
        TimeUpdate();
        ProcessBus();
        SubscriptionsTick(DeviceStatus.UptimeMs); // periodic provider triggers (main loop)

        // Re-sync to the core every ~2-3 min (synchronized-device initiated).
        if ((DeviceStatus.UptimeMs - last_sync_ms) >= next_sync_ms) {
            last_sync_ms = DeviceStatus.UptimeMs;
            next_sync_ms = NODE_TIME_SYNC_INTERVAL_MS + (RawRand() % 60000);
            sendTimeSync();
        }

        DasButtonUpdate(); // Button block edge detection/counter

        // Sample each resistive measurement channel at its own configured rate.
        uint32_t now_ms = Now();
        if ((now_ms - last_sample_ms) >= SampleIntervalMs(Meas1.SamplingRate))
        {
            last_sample_ms = now_ms;
            Measuring_Update(0, &Meas1, Meas_AdcRead(MEAS1_ADC_CH));
        }
        if ((now_ms - last_sample2_ms) >= SampleIntervalMs(Meas2.SamplingRate))
        {
            last_sample2_ms = now_ms;
            Measuring_Update(1, &Meas2, Meas_AdcRead(MEAS2_ADC_CH));
        }

        // Red LED with priority overlays: an active bus error blinks it (~2 Hz), else the
        // identify blink (~10 Hz), else the LED block's LEDState field. The white LED is
        // the RS485 TX activity indicator (driven inside RSBus.h).
        bool red_state;
        if (DasErrorFlag)
            red_state = ((now_ms / 500) & 1) == 0;
        else if (DeviceIdentifyActive(now_ms))
            red_state = ((now_ms / 100) & 1) == 0;
        else
            red_state = DasLed.LEDState;
        if (red_state) PinHigh(LEDR); else PinLow(LEDR);
    }
}