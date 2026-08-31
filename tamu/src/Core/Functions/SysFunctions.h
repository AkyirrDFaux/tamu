#pragma once

#include "Core/Types/Number.h"
#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Packet.h"

// Returns the current SYNCHRONIZED time in milliseconds (raw timer + time offset pushed
// by the core). All scheduling and timestamps use this.
uint32_t Now();
// Returns the RAW time since boot in milliseconds, unaffected by any time offset
// (reported by the Device service Uptime function).
uint32_t TimeFromBoot();
// Blocks for `ms` milliseconds
void Sleep(uint32_t ms);
// Blocks for `us` microseconds
void SleepMicro(uint32_t us);
// Returns the amount of free RAM in bytes
int32_t GetFreeRAM();
// Sends a packet over the bus and waits for acknowledgement; returns true on success
bool SendAndVerifyPacket(const PacketFrame &Data);
// Receives an incoming packet from the bus into `Data`; returns 0 on success
int ReceivePacket(PacketFrame *Data);
// Routes an incoming frame to the local service handler (or forwards it to the bus)
void DispatchPacket(const PacketFrame &frame);
// Defined in Core/Functions/Dispatcher.h; processes the bus queue, dispatching pending received packets
void ProcessBus();

// Rounds `size` up to the nearest multiple of 4 (alignment padding)
inline size_t AlignTo4(size_t size)
{
    return (size + 3) & ~3;
}

// Loop time bookkeeping (defined in Main.cpp)
extern uint32_t LastTime;
extern uint32_t DeltaTime;
// Time offset in ms set by the core via Device service CID 12 (Set time offset)
extern int32_t TimeOffsetMs;

// Updates uptime/delta time bookkeeping and the loop-time statistics each main-loop tick
inline void TimeUpdate()
{
    // Prime on the first call: before any tick, UptimeMs and LastTime are both 0,
    // so a naive DeltaTime would equal the entire boot time and skew the loop stats.
    static bool primed = false;
    if (!primed)
    {
        primed = true;
        LastTime = Now();
        DeviceStatus.UptimeMs = LastTime;
        DeltaTime = 0;
        return;
    }

    LastTime = DeviceStatus.UptimeMs;
    DeviceStatus.UptimeMs = Now();
    DeltaTime = DeviceStatus.UptimeMs - LastTime;

    DeviceStatus.AvgLoopTimeMs = (DeviceStatus.AvgLoopTimeMs * N(0.9375)) + (Number(DeltaTime) * N(0.0625));

    if (Number(DeltaTime) > DeviceStatus.MaxLoopTimeMs)
        DeviceStatus.MaxLoopTimeMs = Number(DeltaTime);

    // Long-term windowing: every 20 s the max decays back to the average. A window
    // counter is deterministic (a `UptimeMs % 20000 < 20` test can be missed entirely
    // when ticks are slower than 20 ms).
    static uint32_t max_window_start = 0;
    if ((uint32_t)(DeviceStatus.UptimeMs - max_window_start) >= 20000)
    {
        max_window_start = DeviceStatus.UptimeMs;
        DeviceStatus.MaxLoopTimeMs = DeviceStatus.AvgLoopTimeMs;
    }
};

