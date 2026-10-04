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

#ifdef USE_APP_INTERFACE
                // A reply's CMD carries the originator's service tag (the echoed TRID). The
                // App owns 0xF000-0xFFFF, and a subscription-management request (set/get/etc.)
                // carries a TRID from the Subscriptions 0x1000-0x1FFF range - both are replies
                // to the app, so hand them back over the app link (the app matches on TRID).
                if (frame.srv_tgt >= TRID_APP_BASE ||
                    (frame.srv_tgt >= TRID_SUB_BASE && frame.srv_tgt <= TRID_SUB_MAX)) {
                    (void)AppInterfaceSend(frame);
                } else
#endif
#ifdef USE_SCRIPTS
                // Script-instance replies (foreign register access): the reply's CMD is the
                // script's slot tag, so it never matches a ServiceType and is routed by range.
                if (frame.srv_tgt >= TRID_SCRIPT_BASE && frame.srv_tgt <= TRID_SCRIPT_MAX) {
                    HandleScriptResponse(frame);
                } else
#endif
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
#if defined(USE_SUB_PROVIDE) || defined(USE_SUB_REQUEST)
                    DeviceLog("DISP", "dispatch Subscriptions cid=%d", cid);
                    HandleSubscriptions(frame);
#endif
                    break;

                case ServiceType::Script:
#ifdef USE_SCRIPTS
                    DeviceLog("DISP", "dispatch Script cid=%d", cid);
                    HandleScript(frame);
#endif
                    break;

                #ifdef USE_APP_INTERFACE
                // Kept as a fallback for legacy 0x11xx app tags; the 0xF000+ range above is
                // the documented App TRID range.
                case ServiceType::App:
                    (void)AppInterfaceSend(frame);
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

// Restores every memory service from its backup file at boot (the static `.SV` space, and
// dynamic blocks when compiled in). Not core-only: nodes with Static Memory (e.g. the DAS
// restores Meas1/Meas2) reuse the same path instead of duplicating it per device.
void LoadAllBackups()
{
    // Static memory: one 1:1 mirror read of `.SV` (the whole persistent space).
    StaticRecallAll();

#ifdef USE_DYNAMIC_BLOCKS
    // Per-block DT/DV files: load every live slot (absent file = tombstone/empty).
    for (uint16_t i = 0; i < MAX_DYNAMIC_BLOCKS; i++) {
        DynamicBlockDescriptor scratch;
        if (!LoadDynamicBlockFiles(scratch, i))
            continue;
        while (dynamic_block_registry.block_count <= i)
            if (!dynamic_block_registry.AddTombstone())
                break;
        if (dynamic_block_registry.block_count > i) {
            dynamic_block_registry.TombstoneBlock(i);
            *dynamic_block_registry.GetBlock(i) = scratch;
        } else {
            scratch.Release();
        }
    }
#endif

#ifdef USE_SUB_REQUEST
    LoadRequesterTable();
#endif
}

