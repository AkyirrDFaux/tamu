#pragma once

#include "Core/Functions/Packet.h"
#ifdef USE_APP_INTERFACE
#include "Core/Functions/AppInterface.h"
#endif
#include "Core/Functions/TrID.h"
#include "Core/Services/Device.h"
#include "Core/Services/LogHandler.h"
#include "Core/Services/Storage.h"
#include "Core/Services/Register.h"
// Backup/restore system needs these (not dispatched as services)
#include "Core/Services/StaticMemory.h"

#ifdef TYPE_CORE
// Log records live in RAM on core devices, on the heap (Docs/Services/Log Handler.md).
// Allocated lazily by EnsureLogStorage() on first use (declared extern in Functions/Log.h).
// LogSeq carries a per-record monotonic sequence number so "drop oldest first" is well
// defined; LogCapacity is the current number of allocated slots.
LogRecord *LogBuffer = nullptr;
bool *LogUsed = nullptr;
uint32_t *LogSeq = nullptr;
uint32_t LogCapacity = 0;
uint32_t LogCount = 0;
#endif

// CLI response handlers are device-specific (implemented per device, e.g. Devices/Tamu_v2.0A/CLI/Handler.h).
// The CLI service is only available on ESP32 devices
#ifdef TYPE_CORE
void HandleCLIService(const PacketFrame &frame);
void HandleCLI_SNDBResponse(const PacketFrame &frame);
void HandleCLI_StatusResponse(const PacketFrame &frame);
void HandleCLI_LogResponse(const PacketFrame &frame);
void HandleCLI_DeviceResponse(const PacketFrame &frame);
void HandleCLI_StorageResponse(const PacketFrame &frame);
void HandleCLI_CreateResponse(const PacketFrame &frame);
void HandleCLI_ScriptResponse(const PacketFrame &frame);
#endif

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
        // TRID response handling: if this is a response (FLAG_TYPE), try to route it
        // through the TRID manager first. The manager will invoke the registered callback
        // and return true if handled.
        if ((frame.flags & FLAG_TYPE) && GlobalTrid.HandleResponse(frame, DeviceStatus.UptimeMs))
        {
            // Response was handled by a registered TRID callback
        }
        else
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

                case ServiceType::Register:
                    HandleRegister(frame);
                    break;

                case ServiceType::Storage:
                    HandleStorageService(frame);
                    break;

                #ifdef USE_APP_INTERFACE
                case ServiceType::App:
                    // Responses to app transactions (SRV TGT type == App, CID = the app's
                    // transaction ID). Forward to the attached app's TX stream; silently
                    // count strays when no link is active.
                    if (!AppInterfaceSend(frame))
                        AppStrayFrames++;
                    break;
                #endif

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
                    else if (cid == 6)
                        HandleCLI_LogResponse(frame); // LogHandler GetLogs/ClearReadLogs responses
                    else if (cid == 7)
                        HandleCLI_ScriptResponse(frame); // Script service responses
                    break;
                #endif

                default:
                    DeviceLog("DISP", "unhandled service %u CID %u", (unsigned)target_srv, (unsigned)cid);
                    ReportLog(MakeLog(false, (uint16_t)target_srv, cid, 0));
                    break;
            }
        } // end else (non-TRID response)
    } // end if (local interest)

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

// Restores every memory service from its backup file at boot (Static, and Dynamic/Keyed
// when compiled in). Not core-only: nodes with Static Memory (e.g. the DAS restores
// Meas1/Meas2) reuse the same path instead of duplicating it per device.
void LoadAllBackups()
{
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t n = ReadBackupFile(StaticLogName(), buf, sizeof(buf));
    if (n > 0)
    {
        for (uint16_t bi = 0; bi < static_block_num; bi++)
        {
            const StaticBlockDescriptor &block = static_block_registry[bi];
            for (uint16_t fi = 0; fi < block.Schema->MapCount; fi++)
            {
                if (block.Schema->Map[fi].FlagsAndType & FieldFlags::ReadOnly)
                    continue;
                const uint8_t *val = nullptr;
                uint8_t vl = 0;
                uint16_t c = 0;
                while (c + kLogEntryHeaderSize <= n)
                {
                    uint8_t b = buf[c];
                    if (b == 0xFF) break;
                    uint8_t sz = buf[c + sizeof(BlockIndex) + 1];
                    uint16_t el = LogEntrySize(sz);
                    if (el > n - c) break;
                    if (b == bi && buf[c + 1] == fi)
                    {
                        val = buf + c + kLogEntryHeaderSize;
                        vl = sz;
                    }
                    c += el;
                }
                if (!val || vl != block.Schema->Map[fi].Size) continue;
                FieldResult fr = block.Get(fi);
                if (fr.Data)
                    memcpy(fr.Data, val, vl);
            }
        }
    }

#ifdef USE_DYNAMIC_MEMORY
    n = ReadBackupFile(DynamicBackupName(), buf, sizeof(buf));
    if (n > 0) DeserializeRegistry(dynamic_block_registry, buf, n);
#endif
}

