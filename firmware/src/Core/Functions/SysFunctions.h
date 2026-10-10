#pragma once

#include "Core/Types/Number.h"
#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Packet.h"

// Returns the current SYNCHRONIZED time in milliseconds (raw timer + this device's time
// offset, learned from the TimeSync reply). All scheduling and timestamps use this.
uint32_t Now();
// Returns the RAW time since boot in milliseconds, unaffected by any time offset
// (reported by the Device service Uptime function).
uint32_t TimeFromBoot();
// Blocks for `ms` milliseconds
void Sleep(uint32_t Ms);
// Blocks for `us` microseconds
void SleepMicro(uint32_t Us);
// Returns the amount of free RAM in bytes
int32_t GetFreeRAM();
// Sends a packet over the bus and waits for acknowledgement; returns true on success
bool SendAndVerifyPacket(const PacketFrame &Data);
// Receives an incoming packet from the bus into `Data`; returns 0 on success
int ReceivePacket(PacketFrame *Data);
// Routes an incoming frame to the local service handler (or forwards it to the bus)
void DispatchPacket(const PacketFrame &Frame);
// Defined in Core/Functions/Dispatcher.h; processes the bus queue, dispatching pending received packets
void ProcessBus();

// Time offset in ms applied by a node after a Device service CID 3 (TimeSync) exchange,
// plus the raw time it was measured at and the offset drift (Q16.16 ms of offset per raw
// ms). Between syncs the offset is extrapolated with the drift so the clock tracks the
// peer's RATE: a node's own oscillator drifts ~1%, so a step-only correction would be
// seconds off again before the next 2-3 minute sync.
extern int32_t TimeOffsetMs;
extern uint32_t TimeOffsetRefRaw;
extern int32_t TimeDrift;

// Clamps the raw-time delta since the reference to bound the Q16.16 drift multiply.
inline int32_t TimeClampDt(uint32_t Raw)
{
    int32_t Dt = (int32_t)(Raw - TimeOffsetRefRaw);
    if (Dt > 1000000) Dt = 1000000;
    else if (Dt < -1000000) Dt = -1000000;
    return Dt;
}

// The drift part of the offset at `raw` (Q16.16 multiply).
inline int32_t TimeOffsetExtrapolation(uint32_t Raw)
{
    return (int32_t)(((int32_t)TimeDrift * TimeClampDt(Raw)) >> 16);
}

// The offset at a raw time: the live offset plus its drift extrapolation.
inline int32_t OffsetAtRaw(uint32_t Raw)
{
    return TimeOffsetMs + TimeOffsetExtrapolation(Raw);
}

// The offset currently applied (for reporting; System field 3.2).
inline int32_t CurrentTimeOffsetMs()
{
    return OffsetAtRaw(TimeFromBoot());
}

// Applies the current offset to a raw timestamp.
inline uint32_t ApplyTimeOffset(uint32_t Raw)
{
    return Raw + (uint32_t)OffsetAtRaw(Raw);
}

// Applies a TimeSync offset measured at the current raw time and updates the drift
// estimate. A large step is treated as a discontinuity (e.g. the peer restarted): the
// offset is stepped but the drift (a property of this device's oscillator) is kept.
inline void ApplyTimeSync(int32_t Offset)
{
    uint32_t Raw = TimeFromBoot();
    int32_t Dt = TimeClampDt(Raw);
    int32_t Target = OffsetAtRaw(Raw) + Offset;

    const bool HaveRef = (TimeOffsetRefRaw != 0);
    if (HaveRef && Dt >= 1000 && Offset <= 5000 && Offset >= -5000)
    {
        int32_t Delta = Target - TimeOffsetMs;
        if (Delta > 30000) Delta = 30000;
        else if (Delta < -30000) Delta = -30000;
        int32_t Slope = (int32_t)(((int32_t)Delta << 16) / Dt); // Q16.16 dO/dR
        if (Slope > 3277) Slope = 3277;        // clamp to +/-5%
        else if (Slope < -3277) Slope = -3277;
        // The estimate is unbiased (it is the true drift plus measurement noise), so use it
        // directly: the next sync corrects any noise.
        TimeDrift = Slope;
    }

    TimeOffsetMs = Target;
    TimeOffsetRefRaw = Raw;
}

// Single shared output buffer — all handlers build replies here instead of stack-allocating.
extern PacketFrame TxFrame;

// Updates uptime/delta time bookkeeping and the loop-time statistics each main-loop tick
inline void TimeUpdate()
{
    // Loop-time bookkeeping (function-local so no globals leak out of this helper).
    static bool Primed = false;
    if (!Primed)
    {
        // Prime on the first call: before any tick, UptimeMs is 0, so a naive delta
        // would equal the entire boot time and skew the loop stats.
        Primed = true;
        DeviceStatus.UptimeMs = Now();
        return;
    }

    uint32_t Prev = DeviceStatus.UptimeMs;
    DeviceStatus.UptimeMs = Now();
    uint32_t Delta = DeviceStatus.UptimeMs - Prev;

    DeviceStatus.AvgLoopTimeMs = (DeviceStatus.AvgLoopTimeMs * N(0.9375)) + (Number(Delta) * N(0.0625));

    if (Number(Delta) > DeviceStatus.MaxLoopTimeMs)
        DeviceStatus.MaxLoopTimeMs = Number(Delta);

    // Long-term windowing: every 20 s the max decays back to the average. A window
    // counter is deterministic (a `UptimeMs % 20000 < 20` test can be missed entirely
    // when ticks are slower than 20 ms).
    static uint32_t MaxWindowStart = 0;
    if ((uint32_t)(DeviceStatus.UptimeMs - MaxWindowStart) >= 20000)
    {
        MaxWindowStart = DeviceStatus.UptimeMs;
        DeviceStatus.MaxLoopTimeMs = DeviceStatus.AvgLoopTimeMs;
    }
};

