#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Device.h"
#include "Core/Functions/SysFunctions.h"
#include "Core/Types/Number.h" // RawRand() for interval jitter

#ifdef TYPE_CORE

// Core time synchronization (Docs/Services/System Block and Device Commands.md).
//
// TimeSync is SYNCHRONIZED-DEVICE INITIATED: a device that wants to be synced sends
// Device service CID 3 (its local time); the peer answers with (echo, receive time, send
// time); the INITIATOR computes the offset locally and applies it to its own clock. The
// core never pushes offsets to other devices - it only RESPONDS to a node's TimeSync, and
// syncs ITSELF to the longest-running core on the bus.
//
// The core finds that reference core with Core discover (CID 10, broadcast to 3F.1),
// remembers the responder with the highest uptime, then TimeSyncs to it. The CID 3
// response path (Core/Services/Device.h) applies the resulting offset to this core.
class CoreTimeSyncService
{
public:
    static const uint32_t DISCOVER_INTERVAL_MS = 150000; // ~2.5 min (docs "2-3 minutes")
    static const uint32_t DISCOVER_WINDOW_MS   = 500;    // docs: responses within 500 ms

    // Scheduling jitter: +/-10% around the nominal interval, so synchronised cores do not
    // re-sync in lockstep and periodically swamp the reference core (Docs/Services/System
    // Block and Device Commands.md: the core "re-syncs ... at intervals ... jittered so the
    // reference core is not periodically overwhelmed").
    static const uint32_t DISCOVER_JITTER_MS = DISCOVER_INTERVAL_MS / 10;

    // Main-loop tick: starts a discovery round when due and closes it after the window.
    // `now_ms` must be RAW time (TimeFromBoot): a clock correction changes
    // DeviceStatus.UptimeMs, and scheduling against it would let the correction itself
    // trigger the next round.
    void Tick(uint32_t NowMs)
    {
        if (CurrentPhase == Idle)
        {
            if ((int32_t)(NowMs - DueMs) >= 0)
                StartDiscover(NowMs);
        }
        else if ((int32_t)(NowMs - DeadlineMs) >= 0)
        {
            FinishDiscover(NowMs);
        }
    }

    // Called for every Core-discover RESPONSE (Device service CID 10).
    void HandleDiscoverResponse(const PacketFrame &Frame)
    {
        if (PayloadBytes(Frame) < 14 + 4)
            return;
        const SerialNumber *Sn = reinterpret_cast<const SerialNumber *>(Frame.Payload);
        if (*Sn == GetSerialNumber())
            return; // our own broadcast echoed back by the local dispatch

        // Net-ID collision (Docs "Core functions"): a different core answering with OUR net
        // claimed the net twice. Checked here so boot-time discovery (phase == Idle) catches
        // it too, not just the periodic round.
        NoteCoreNet((uint8_t)((Frame.IdSrc >> 10) & 0x3F), DeviceStatus.NetId);

        if (CurrentPhase != Discovering)
            return;
        uint32_t Uptime = 0;
        memcpy(&Uptime, Frame.Payload + 14, sizeof(Uptime));
        if (Uptime > BestUptime)
        {
            BestUptime = Uptime;
            BestAddr = Frame.IdSrc;
        }
    }

private:
    enum Phase : uint8_t { Idle, Discovering };

    Phase CurrentPhase = Idle;
    uint32_t DueMs = 0;
    uint32_t DeadlineMs = 0;
    uint16_t BestAddr = 0;
    uint32_t BestUptime = 0;

    void StartDiscover(uint32_t NowMs)
    {
        BestAddr = 0;
        BestUptime = 0;
        PacketConstruct(&TxFrame, ADDR_ALL_CORES,
                        MakeService(ServiceType::Device, 0x10),
                        NextSystemTrid(ServiceType::Device),
                        FLAG_REQACK | FLAG_START | FLAG_STOP, nullptr, 0);
        DispatchPacket(TxFrame);
        DeadlineMs = NowMs + DISCOVER_WINDOW_MS;
        CurrentPhase = Discovering;
    }

    void FinishDiscover(uint32_t NowMs)
    {
        CurrentPhase = Idle;
        // Jittered interval: uniform over [nominal - 10%, nominal + 10%]. RawRand() is the
        // shared codebase PRNG (Core/Types/Number.h) - no new dependency. Still scheduled
        // from the raw now_ms passed in, so a clock correction cannot trigger the next round.
        DueMs = NowMs + DISCOVER_INTERVAL_MS - DISCOVER_JITTER_MS
                 + (RawRand() % (2 * DISCOVER_JITTER_MS + 1));
        if (BestAddr == 0 || BestAddr == DeviceStatus.ShortAddress)
            return; // no other core on the bus: this core is the reference

        // NTP-like: send the SYNCHRONIZED local time so the reply's offset is a
        // correction (delta) to add, not an absolute value to overwrite.
        uint32_t SentTime = Now();
        PacketConstruct(&TxFrame, BestAddr,
                        MakeService(ServiceType::Device, 3),
                        NextSystemTrid(ServiceType::Device),
                        FLAG_REQACK | FLAG_START | FLAG_STOP,
                        (const uint8_t *)&SentTime, sizeof(SentTime),
                        PRIORITY_TIMESYNC);
        DispatchPacket(TxFrame);
    }
};

CoreTimeSyncService CoreTimeSync;

#endif // TYPE_CORE
