#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Services/Device.h"
#include "Core/Services/LogHandler.h"
#ifdef USE_SCRIPT
#include "Core/Services/Script.h"
#endif
#include "Core/Services/Storage.h"
#include "Core/Services/SystemMemory.h"
#ifdef USE_DYNAMIC_MEMORY
#include "Core/Services/DynamicMemory.h"
#endif
#ifdef USE_KEYED_MEMORY
#include "Core/Services/KeyedMemory.h"
#endif

#ifdef TYPE_CORE
// Log records live in RAM on core devices, on the heap (Docs/Services/Log Handler.md).
// Allocated lazily by EnsureLogStorage() on first use (declared extern in Functions/Log.h).
LogRecord *LogBuffer = nullptr;
bool *LogUsed = nullptr;
#endif

// CLI response handlers are device-specific (implemented per device, e.g. Devices/Tamu_v2.0A/CLI/Handler.h).
// The CLI service is only available on ESP32 devices
#ifdef ESP32
void HandleCLIService(const PacketFrame &frame);
void HandleCLI_SNDBResponse(const PacketFrame &frame);
void HandleCLI_StatusResponse(const PacketFrame &frame);
void HandleCLI_DeviceResponse(const PacketFrame &frame);
void HandleCLI_StorageResponse(const PacketFrame &frame);
void HandleCLI_CreateResponse(const PacketFrame &frame);
#endif

// Forward declaration for HandleSystemMemory (defined in Core/Services/SystemMemory.h).
void HandleSystemMemory(const PacketFrame &frame);

// The Main Packet Dispatcher: routes an incoming frame to the local service
// handler (or forwards it to the bus when it targets another device).
void DispatchPacket(const PacketFrame &frame)
{
    // 1. Validation: Drop invalid packets
    if (frame.id_tgt == ADDR_INVALID)
        return;

    // TODO: Router tree topology (Docs/Services/Router.md)
    // - per-RSBus-port ID table, appended/updated on incoming packets
    // - broadcast to all directions when the target is unknown
    // Currently the bus is a single shared medium, so only self/forward
    // handling below is implemented.

    // 2. Determine "Local Interest"
    if ((frame.id_tgt == DeviceStatus.ShortAddress) || (frame.id_tgt == ADDR_BROADCAST))
    {
        ServiceType target_srv = GetServiceType(frame.srv_tgt);
        uint8_t cid = GetServiceCID(frame.srv_tgt);

        switch (target_srv)
        {
            case ServiceType::Device:
                HandleDeviceService(frame);
                break;

            case ServiceType::LogHandler:
                HandleLogHandler(frame);
                break;

            case ServiceType::SystemMemory:
                HandleSystemMemory(frame);
                break;

            #ifdef USE_SCRIPT
            case ServiceType::Script:
                HandleScriptService(frame);
                break;
#endif

#ifdef USE_DYNAMIC_MEMORY
            case ServiceType::DynamicMemory:
                HandleDynamicMemory(frame);
                break;
#endif

#ifdef USE_KEYED_MEMORY
            case ServiceType::KeyedMemory:
                HandleKeyedMemory(frame);
                break;
#endif

            case ServiceType::Storage:
                HandleStorageService(frame);
                break;

            #ifdef TYPE_CORE
            case ServiceType::CLI:
                if (cid == 0)
                    HandleCLIService(frame);   // Memory read responses
                else if (cid == 1)
                    HandleCLI_SNDBResponse(frame); // SNDB responses
                else if (cid == 2)
                    HandleCLI_StatusResponse(frame); // Save/Recall/Delete status responses
                else if (cid == 3)
                    HandleCLI_DeviceResponse(frame); // Device service responses
                else if (cid == 4)
                    HandleCLI_StorageResponse(frame); // Storage service responses
                else if (cid == 5)
                    HandleCLI_CreateResponse(frame); // Dynamic/Keyed create responses
                break;
#endif

            default:
                ReportLog(MakeLog(false, (uint16_t)target_srv, cid, 0));
                break;
        }
    }

    // Forwarding Rule: Always forward to physical bus if the target is external and it originated locally,
    // or if we are a router node and need to route packages to other devices.
    if (frame.id_tgt != DeviceStatus.ShortAddress && frame.id_src == DeviceStatus.ShortAddress)
    {
        SendAndVerifyPacket(frame);
    }
}

// Processes the bus input queue, dispatching one received frame per call to the local
// handlers (the main loop calls this repeatedly to drain the queue).
void ProcessBus()
{
    static PacketFrame rx_frame __attribute__((aligned(4)));
    int frame_size = ReceivePacket(&rx_frame);
    if (frame_size == 0)
    {
        return;
    }
    DispatchPacket(rx_frame);
}

#ifdef TYPE_CORE
// Restores every memory service from its backup file at boot (System, Dynamic, Keyed).
void LoadAllBackups()
{
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t n = ReadBackupFile(SystemBackupName(), buf, sizeof(buf));
    if (n > 0) DeserializeSystemBlocks(buf, n);

#ifdef USE_DYNAMIC_MEMORY
    n = ReadBackupFile(DynamicBackupName(), buf, sizeof(buf));
    if (n > 0) DeserializeRegistry(dynamic_block_registry, buf, n);
#endif

#ifdef USE_KEYED_MEMORY
    n = ReadBackupFile(KeyedBackupName(), buf, sizeof(buf));
    if (n > 0) DeserializeRegistry(keyed_block_registry, buf, n);
#endif
}
#endif

