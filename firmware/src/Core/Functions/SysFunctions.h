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

// Time offset in ms applied by a node after a Device service CID 3 (TimeSync) exchange,
// plus the raw time it was measured at and the offset drift (Q16.16 ms of offset per raw
// ms). Between syncs the offset is extrapolated with the drift so the clock tracks the
// peer's RATE: a node's own oscillator drifts ~1%, so a step-only correction would be
// seconds off again before the next 2-3 minute sync.
extern int32_t TimeOffsetMs;
extern uint32_t TimeOffsetRefRaw;
extern int32_t TimeDrift;

// The drift part of the offset at `raw` (Q16.16 multiply, clamped so it cannot overflow).
inline int32_t TimeOffsetExtrapolation(uint32_t raw)
{
    int32_t dt = (int32_t)(raw - TimeOffsetRefRaw);
    if (dt > 1000000) dt = 1000000; // clamp: bounds the Q16.16 multiply
    else if (dt < -1000000) dt = -1000000;
    return (int32_t)(((int32_t)TimeDrift * dt) >> 16);
}

// The offset currently applied (for reporting; System field 3.2).
inline int32_t CurrentTimeOffsetMs()
{
    return TimeOffsetMs + TimeOffsetExtrapolation(TimeFromBoot());
}

// Applies the current offset to a raw timestamp.
inline uint32_t ApplyTimeOffset(uint32_t raw)
{
    return raw + (uint32_t)(TimeOffsetMs + TimeOffsetExtrapolation(raw));
}

// Applies a TimeSync offset measured at the current raw time and updates the drift
// estimate. A large step is treated as a discontinuity (e.g. the peer restarted): the
// offset is stepped but the drift (a property of this device's oscillator) is kept.
inline void ApplyTimeSync(int32_t offset)
{
    uint32_t r = TimeFromBoot();
    int32_t dt = (int32_t)(r - TimeOffsetRefRaw);
    if (dt > 1000000) dt = 1000000;
    else if (dt < -1000000) dt = -1000000;

    int32_t applied = TimeOffsetMs + (int32_t)(((int32_t)TimeDrift * dt) >> 16);
    int32_t target = applied + offset;

    const bool haveRef = (TimeOffsetRefRaw != 0);
    if (haveRef && dt >= 1000 && offset <= 5000 && offset >= -5000)
    {
        int32_t delta = target - TimeOffsetMs;
        if (delta > 30000) delta = 30000;
        else if (delta < -30000) delta = -30000;
        int32_t slope = (int32_t)(((int32_t)delta << 16) / dt); // Q16.16 dO/dR
        if (slope > 3277) slope = 3277;        // clamp to +/-5%
        else if (slope < -3277) slope = -3277;
        // The estimate is unbiased (it is the true drift plus measurement noise), so use it
        // directly: the next sync corrects any noise.
        TimeDrift = slope;
    }

    TimeOffsetMs = target;
    TimeOffsetRefRaw = r;
}

// Single shared output buffer — all handlers build replies here instead of stack-allocating.
extern PacketFrame tx_frame;

// Updates uptime/delta time bookkeeping and the loop-time statistics each main-loop tick
inline void TimeUpdate()
{
    // Loop-time bookkeeping (function-local so no globals leak out of this helper).
    static bool primed = false;
    if (!primed)
    {
        // Prime on the first call: before any tick, UptimeMs is 0, so a naive delta
        // would equal the entire boot time and skew the loop stats.
        primed = true;
        DeviceStatus.UptimeMs = Now();
        return;
    }

    uint32_t prev = DeviceStatus.UptimeMs;
    DeviceStatus.UptimeMs = Now();
    uint32_t delta = DeviceStatus.UptimeMs - prev;

    DeviceStatus.AvgLoopTimeMs = (DeviceStatus.AvgLoopTimeMs * N(0.9375)) + (Number(delta) * N(0.0625));

    if (Number(delta) > DeviceStatus.MaxLoopTimeMs)
        DeviceStatus.MaxLoopTimeMs = Number(delta);

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

