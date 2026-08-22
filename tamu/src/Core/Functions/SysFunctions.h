#pragma once

#include "Core/Types/Number.h"
#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Packet.h"

// Returns the current time in milliseconds
uint32_t Now();
// Blocks for `ms` milliseconds
void Sleep(uint32_t ms);
// Blocks for `us` microseconds
void SleepMicro(uint32_t us);
// Returns the total system RAM in bytes
int32_t GetRAM();
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
// Time offset in ms set by the core via Device service CID 11 (Set time offset)
extern int32_t TimeOffsetMs;

// Updates uptime/delta time bookkeeping and the loop-time statistics each main-loop tick
inline void TimeUpdate()
{
    // Prime on the first call: before any tick, UptimeMs and LastTime are both 0,
    // so a naive DeltaTime would equal the entire boot time and skew the loop stats.
    if (DeviceStatus.UptimeMs == 0 && LastTime == 0)
    {
        LastTime = Now() + TimeOffsetMs;
        DeviceStatus.UptimeMs = LastTime;
        DeltaTime = 0;
        return;
    }

    LastTime = DeviceStatus.UptimeMs;
    DeviceStatus.UptimeMs = Now() + TimeOffsetMs;
    DeltaTime = DeviceStatus.UptimeMs - LastTime;

    DeviceStatus.AvgLoopTimeMs = (DeviceStatus.AvgLoopTimeMs * N(0.9375)) + (Number(DeltaTime) * N(0.0625));

    if (Number(DeltaTime) > DeviceStatus.MaxLoopTimeMs)
        DeviceStatus.MaxLoopTimeMs = Number(DeltaTime);
    else if (DeviceStatus.UptimeMs % 20000 < 20)
        DeviceStatus.MaxLoopTimeMs = DeviceStatus.AvgLoopTimeMs; // Reset max to current avg for long-term windowing
};

