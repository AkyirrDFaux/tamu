#include "ch32v00x.h"
#include "debug.h"

// NOTE: DeviceLog/DeviceLogHex are compiled out via no-op macros in src/Main.cpp
// (DEVICE_LOG_TEXTLESS) - the DAS log format carries no free text, see Log.h.

// Forward declaration for LoadAllBackups function
void LoadAllBackups();

#include "Base.h"
#include "RSBus.h"
#include "Log.h"
#include "Storage.h"
#include "Measuring.h"
#include "Core/Functions/Device.h"
#include "Core/Services/SystemMemory.h"

#define CHIP_ID_ADDR  0x1FFFF7E8 // Fixed memory-mapped location of the chip's unique ID

// Restores the saved System Memory values (Meas1/Meas2 writable fields) from the backup
// file at boot, the same way the core does. The buffer size follows MEMORY_BACKUP_CAP,
// which the DAS build keeps small (256) to fit the 2 KB RAM.
void LoadAllBackups() {
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t n = ReadBackupFile(SystemBackupName(), buf, sizeof(buf));
    if (n > 0) DeserializeSystemBlocks(buf, n);
}

// Device identity (mandatory, see Core/Functions/Device.h).
extern const DeviceType kDeviceType = DeviceType::DualAnalogSensor;
extern const uint32_t kCapabilities = Capabilities::None; // plain node: no core capability

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

ResistiveMeasStruct Meas1;
ResistiveMeasStruct Meas2;

const StaticBlockDescriptor static_block_registry[] = {
    {&Meas1, &ResistiveMeas_Schema, "Meas1"},
    {&Meas2, &ResistiveMeas_Schema, "Meas2"},
};
const size_t static_block_num = sizeof(static_block_registry) / sizeof(StaticBlockDescriptor);

const char* DeviceVersion = "DAS v0.1";

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

    SetupRS485();
    Measuring_Init();

    // Persistent storage: formats the flash directory if needed and restores the saved
    // system state. The short address is cleared afterwards so a changed network always
    // gets a fresh registration (SNumber/name persistence still works).
    Storage.Init();
    LoadAllBackups();
    DeviceStatus.ShortAddress = 0;

    // Core distinction loop
    while (DeviceStatus.ShortAddress == 0)
    {
        PacketFrame frame;
        PacketConstruct(&frame, ADDR_BROADCAST,
                         MakeService(ServiceType::Device, 0),
                         MakeService(ServiceType::Device, 0),
                         FLAG_REQACK | FLAG_START | FLAG_STOP,
                         (const uint8_t *)&GetSerialNumber(), sizeof(SerialNumber));
        DispatchPacket(frame);
        Sleep(500);
        ProcessBus();
    }
    PinLow(LEDR);

    uint32_t last_sample_ms = 0;
    uint32_t last_sample2_ms = 0;
    uint32_t last_blink_ms = 0;
    bool blink_high = false;

    while (1)
    {
        TimeUpdate();
        ProcessBus();

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

        // Non-blocking LED blink so ProcessBus keeps servicing the bus every loop.
        if ((now_ms - last_blink_ms) >= 500)
        {
            last_blink_ms = now_ms;
            blink_high = !blink_high;
            if (blink_high) PinHigh(LEDR); else PinLow(LEDR);
        }
    }
}