#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Device.h"
#include "Core/Functions/SysFunctions.h"

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

    // Main-loop tick: starts a discovery round when due and closes it after the window.
    void Tick(uint32_t now_ms)
    {
        if (phase == Idle)
        {
            if ((int32_t)(now_ms - due_ms) >= 0)
                StartDiscover(now_ms);
        }
        else if ((int32_t)(now_ms - deadline_ms) >= 0)
        {
            FinishDiscover();
        }
    }

    // Called for every Core-discover RESPONSE (Device service CID 10).
    void HandleDiscoverResponse(const PacketFrame &frame)
    {
        if (phase != Discovering)
            return;
        if (PayloadBytes(frame) < 14 + 4)
            return;
        const SerialNumber *sn = reinterpret_cast<const SerialNumber *>(frame.payload);
        if (*sn == GetSerialNumber())
            return; // our own broadcast echoed back by the local dispatch
        uint32_t uptime = 0;
        memcpy(&uptime, frame.payload + 14, sizeof(uptime));
        if (uptime > best_uptime)
        {
            best_uptime = uptime;
            best_addr = frame.id_src;
        }
    }

private:
    enum Phase : uint8_t { Idle, Discovering };

    Phase phase = Idle;
    uint32_t due_ms = 0;
    uint32_t deadline_ms = 0;
    uint16_t best_addr = 0;
    uint32_t best_uptime = 0;

    void StartDiscover(uint32_t now_ms)
    {
        best_addr = 0;
        best_uptime = 0;
        PacketConstruct(&tx_frame, ADDR_ALL_CORES,
                        MakeService(ServiceType::Device, 10),
                        MakeService(ServiceType::Device, 10),
                        FLAG_REQACK | FLAG_START | FLAG_STOP, nullptr, 0);
        DispatchPacket(tx_frame);
        deadline_ms = now_ms + DISCOVER_WINDOW_MS;
        phase = Discovering;
    }

    void FinishDiscover()
    {
        phase = Idle;
        due_ms = DeviceStatus.UptimeMs + DISCOVER_INTERVAL_MS;
        if (best_addr == 0 || best_addr == DeviceStatus.ShortAddress)
            return; // no other core on the bus: this core is the reference

        uint32_t sent_time = TimeFromBoot();
        PacketConstruct(&tx_frame, best_addr,
                        MakeService(ServiceType::Device, 3),
                        MakeService(ServiceType::Device, 3),
                        FLAG_REQACK | FLAG_START | FLAG_STOP,
                        (const uint8_t *)&sent_time, sizeof(sent_time));
        DispatchPacket(tx_frame);
    }
};

CoreTimeSyncService CoreTimeSync;

#endif // TYPE_CORE
