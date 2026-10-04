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

    // TODO (Router): multi-bus tree topology is NOT implemented - see
    // Docs/Services/Router.md:9 ("Not to be implemented yet, no multi-bus device
    // available"). The bus is a single shared medium, so only self/forward handling
    // below exists: a per-RSBus-port ID table and broadcast-to-all-directions would
    // be added here if a multi-bus device appears.

    // 2. Determine "Local Interest"
    bool all_cores = false;
#ifdef TYPE_CORE
    all_cores = (frame.id_tgt == ADDR_ALL_CORES); // 3F.1 Core-discover broadcast
#endif
    if (all_cores || (NetQualifyLocal(frame.id_tgt, DeviceStatus.NetId) == DeviceStatus.ShortAddress) ||
        (frame.id_tgt == ADDR_BROADCAST))
    {
        DeviceLog("DISP", "local interest id_tgt=%d id_src=%d srv_tgt=0x%04X trid=0x%04X", frame.id_tgt, frame.id_src, frame.srv_tgt, frame.trid);
        // A reply's CMD is the originator's TRID (Docs "Transaction IDs"). The System/Log TRID
        // keeps the service type in its high byte, so a Device/Log reply routes by
        // GetServiceType; a subscription install reply carries a Subscriptions TRID and is
        // range-routed to the requester; App (0xF000-0xFFFF) and Scripts (0x2000-0x2FFF) too.
        ServiceType target_srv = GetServiceType(frame.srv_tgt);
        uint8_t cid = GetServiceCID(frame.srv_tgt);
        bool is_response = (frame.flags & FLAG_TYPE) != 0;
        DeviceLog("DISP", "dispatching to service=%d cid=%d", target_srv, cid);

        bool handled = false;
#ifdef USE_SUB_REQUEST
        // A response whose CMD is a subscription TRID (not a service/CID) is a remote
        // provider's reply to a requester-originated install: it carries the source's current
        // value and must reach the requester, not the app link. A locally generated management
        // reply (our own id_src) or a FLAG_SUCCESS/FAIL ack stays with the app.
        if (is_response && (frame.flags & (FLAG_SUCCESS | FLAG_FAIL)) == 0 &&
            frame.srv_tgt >= TRID_SUB_BASE && frame.srv_tgt <= TRID_SUB_MAX &&
            frame.id_src != DeviceStatus.ShortAddress) {
            HandleSubscriptions(frame);
            handled = true;
        }
#endif
#ifdef USE_APP_INTERFACE
        // A subscription-management reply carries a TRID from the Subscriptions range; both
        // it and an App reply (whose CMD is the app's 0xF000-0xFFFF TRID) go back over the
        // app link, where the app matches on TRID.
        if (!handled &&
            (frame.srv_tgt >= TRID_APP_BASE ||
             (frame.srv_tgt >= TRID_SUB_BASE && frame.srv_tgt <= TRID_SUB_MAX))) {
            (void)AppInterfaceSend(frame);
            handled = true;
        }
#endif
#ifdef USE_SCRIPTS
        // Script-instance replies (foreign register access): the reply's CMD is the
        // script's slot tag, so it never matches a ServiceType and is routed by range.
        if (!handled && frame.srv_tgt >= TRID_SCRIPT_BASE && frame.srv_tgt <= TRID_SCRIPT_MAX) {
            HandleScriptResponse(frame);
            handled = true;
        }
#endif
        if (!handled) switch (target_srv)
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

        default:
            DeviceLog("DISP", "unhandled service %u CID %u", (unsigned)target_srv, (unsigned)cid);
            ReportLog(MakeLog(false, (uint16_t)target_srv, cid, 0));
            break;
        }
    } // end if (local interest)

    // Forwarding Rule: Always forward to physical bus if the target is external and it
    // originated locally. (The app link's frames are rewritten to our own short address
    // by AppInterfaceRouteIn, so they forward through the same path.)
    if (NetQualifyLocal(frame.id_tgt, DeviceStatus.NetId) != DeviceStatus.ShortAddress &&
        frame.id_src == DeviceStatus.ShortAddress)
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
    for (uint16_t i = 0; i < MAX_DYNAMIC_BLOCKS; i++)
        RestoreDynamicBlock(i);
#endif

#ifdef USE_SUB_REQUEST
    LoadRequesterTable();
#endif
}

