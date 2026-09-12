#pragma once

#include "Core/Functions/Packet.h"
#ifdef USE_APP_INTERFACE
#include "Core/Functions/AppInterface.h"
#endif
#include "Core/Services/Device.h"
#include "Core/Services/LogHandler.h"
#include "Core/Services/Storage.h"
#include "Core/Services/Register.h"
#include "Core/Services/Subscriptions.h"
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
void HandleCLI_RegisterResponse(const PacketFrame &frame);
void HandleCLI_SNDBResponse(const PacketFrame &frame);
void HandleCLI_StatusResponse(const PacketFrame &frame);
void HandleCLI_DeviceResponse(const PacketFrame &frame);
void HandleCLI_StorageResponse(const PacketFrame &frame);
void HandleCLI_CreateResponse(const PacketFrame &frame);
void HandleCLI_LogResponse(const PacketFrame &frame);
void HandleCLI_SubsResponse(const PacketFrame &frame);
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
    bool all_cores = false;
#ifdef TYPE_CORE
    all_cores = (frame.id_tgt == ADDR_ALL_CORES); // 3F.1 Core-discover broadcast
#endif
    if (all_cores || (frame.id_tgt == DeviceStatus.ShortAddress) || (frame.id_tgt == ADDR_BROADCAST) || (frame.id_tgt == 0xFFFE))
    {
        DeviceLog("DISP", "local interest id_tgt=%d id_src=%d srv_tgt=0x%04X trid=0x%04X", frame.id_tgt, frame.id_src, frame.srv_tgt, frame.trid);
        // Responses (FLAG_TYPE) are routed straight to the target service, which
        // resolves them by command/CID (e.g. Discover, TimeSync, SNDB, Logs).
        // (The documented TRID-handler table is not used by this implementation.)
        // For frames destined to the app (0xFFFE), route directly to App interface
        if (frame.id_tgt == 0xFFFE)
            {
                #ifdef USE_APP_INTERFACE
                AppInterfaceSend(frame);
                #endif
            }
            else
            {
                ServiceType target_srv = GetServiceType(frame.srv_tgt);
                uint8_t cid = GetServiceCID(frame.srv_tgt);
                DeviceLog("DISP", "dispatching to service=%d cid=%d", target_srv, cid);

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

                case ServiceType::Subscriptions:
#ifndef BOARD_DAS_v0_1
                    DeviceLog("DISP", "dispatch Subscriptions cid=%d", cid);
                    HandleSubscriptions(frame);
#endif
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
                        HandleCLI_RegisterResponse(frame);   // Memory read responses
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
                    else if (cid == 8)
                        HandleCLI_SubsResponse(frame); // Subscriptions service responses
                    break;
                #endif

                default:
                    DeviceLog("DISP", "unhandled service %u CID %u", (unsigned)target_srv, (unsigned)cid);
                    ReportLog(MakeLog(false, (uint16_t)target_srv, cid, 0));
                    break;
            }
        } // end else (id_tgt != 0xFFFE)
    } // end if (local interest)

    // Forwarding Rule: Always forward to physical bus if the target is external and it originated locally,
    // or if we are a router node and need to route packages to other devices.
    // Also forward packets from the App interface (id_src == 0xFFFE) to the bus.
    if (frame.id_tgt != DeviceStatus.ShortAddress && 
        (frame.id_src == DeviceStatus.ShortAddress || frame.id_src == 0xFFFE))
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
    // Remove the obsolete standalone name/net-id files - the System block's Name and
    // NetID now persist through the STATLOG mirror (like every other persistent field).
    static const char devname_file[8] = {'D','E','V','N','A','M','E',' '};
    static const char netid_file[8]  = {'N','E','T','I','D',' ',' ',' '};
    if (Storage.FileExists(devname_file) != 0xFFFFFFFF) Storage.DeleteFile(devname_file);
    if (Storage.FileExists(netid_file) != 0xFFFFFFFF) Storage.DeleteFile(netid_file);

    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t n = ReadBackupFile(StaticLogName(), buf, sizeof(buf));
    if (n > 0)
    {
        for (uint16_t bi = 0; bi < static_block_num; bi++)
        {
            const StaticBlockDescriptor &block = static_block_registry[bi];
            for (uint16_t fi = 0; fi < block.Schema->MapCount; fi++)
            {
                // Only writable Persistent fields are retained after reboot (Register.md);
                // volatile and read-only fields are never restored from the backup.
                if (!(block.Schema->Map[fi].FlagsAndType & FieldFlags::Persistent) ||
                    (block.Schema->Map[fi].FlagsAndType & FieldFlags::ReadOnly))
                    continue;
                const uint8_t *val = nullptr;
                uint8_t vl = 0;
                uint16_t c = 0;
                while (c + kLogEntryHeaderSize <= n)
                {
                    uint8_t b = buf[c];
                    if (b == 0xFF) break;
                    // Entry layout: BlockIndex[4] + BlockMeta[4] + Value. The value
                    // size lives in the BlockMeta.Size byte, the LAST byte of the
                    // 8-byte header (c + kLogEntryHeaderSize - 1).
                    uint8_t sz = buf[c + kLogEntryHeaderSize - 1];
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

        // System block persistent fields (Name 6, NetID 7) live in the same STATLOG
        // mirror under the SYSTEM_BLOCK_BACKUP marker (no separate DEVNAME/NETID files).
        for (uint16_t c = 0; c + kLogEntryHeaderSize <= n; )
        {
            uint8_t b = buf[c];
            if (b == 0xFF) break;
            uint8_t sz = buf[c + kLogEntryHeaderSize - 1];
            uint16_t el = LogEntrySize(sz);
            if (c + el > n) break;
            if (b == SYSTEM_BLOCK_BACKUP)
            {
                uint8_t field = buf[c + 1];
                uint16_t val_c = c + kLogEntryHeaderSize;
                if (field == SYSTEM_FIELD_NAME)
                {
                    uint16_t nl = sz; if (nl > 16) nl = 16;
                    memcpy(DeviceNameBuffer, buf + val_c, nl);
                    DeviceNameBuffer[nl] = '\0';
                }
                else if (field == SYSTEM_FIELD_NETID)
                {
                    DeviceStatus.NetId = buf[val_c];
                }
            }
            c += el;
        }
    }

#ifndef DISABLE_DYNAMIC_MEMORY
    n = ReadBackupFile(DynamicBackupName(), buf, sizeof(buf));
    if (n > 0) DeserializeRegistry(dynamic_block_registry, buf, n);
#endif

#ifndef BOARD_DAS_v0_1
    LoadRequesterTable();
#endif
}

