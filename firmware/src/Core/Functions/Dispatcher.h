#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/BusRepeat.h" // repeat guard: a retransmission must not execute twice
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
void DispatchPacket(const PacketFrame &Frame)
{
    // 1. Validation: Drop invalid packets
    if (Frame.IdTgt == ADDR_INVALID)
        return;

    // TODO (Router): multi-bus tree topology is NOT implemented - see
    // Docs/Services/Router.md:9 ("Not to be implemented yet, no multi-bus device
    // available"). The bus is a single shared medium, so only self/forward handling
    // below exists: a per-RSBus-port ID table and broadcast-to-all-directions would
    // be added here if a multi-bus device appears.

    // 2. Determine "Local Interest"
    bool AllCores = false;
#ifdef TYPE_CORE
    AllCores = (Frame.IdTgt == ADDR_ALL_CORES); // 3F.1 Core-discover broadcast
#endif
    if (AllCores || (NetQualifyLocal(Frame.IdTgt, DeviceStatus.NetId) == DeviceStatus.ShortAddress) ||
        (Frame.IdTgt == ADDR_BROADCAST))
    {
        // A response echoes the request's TRID (Docs "Transaction IDs") and carries the
        // request's CMD, so GetServiceType names the replying service. A System reply
        // (0x0000-0x0FFF) therefore routes by CMD; the Subscriptions, App and Script ranges
        // route by the echoed TRID, which each owner allocated.
        ServiceType TargetSrv = GetServiceType(Frame.Cmd);
        uint8_t Cid = GetServiceCID(Frame.Cmd);
        DeviceLog("DISP", "local interest IdTgt=%d IdSrc=%d Cmd=0x%04X Trid=0x%04X -> service=%d cid=%d",
                  Frame.IdTgt, Frame.IdSrc, Frame.Cmd, Frame.Trid, TargetSrv, Cid);
#ifdef USE_APP_INTERFACE
        // A response from another device is the answer the open transaction was waiting for (a
        // reply, or the confirmation a confirm-required value update asked for): it ends the
        // transaction slot (AppInterface.h AppBusForwardPending), which is what lets the next
        // queued frame go out. The TRID match inside the call decides, so this is asked for every
        // frame from another device - not only a FLAG_TYPE one, or a value update's confirmation
        // would leave the slot open until its window expired.
        if (Frame.IdSrc != DeviceStatus.ShortAddress)
            AppBusForwardSawReply(Frame.Trid);
#endif
        // Only the gated routing blocks below read this, so declare it under the same guard;
        // a target with none of them enabled (a DAS node) must not carry an unused local.
#if defined(USE_SUB_REQUEST) || defined(USE_APP_INTERFACE) || defined(USE_SCRIPTS)
        bool IsResponse = (Frame.Flags & FLAG_TYPE) != 0;
#endif

        bool Handled = false;
#ifdef USE_SUB_REQUEST
        // A remote provider's value update echoes the requester's Subscriptions TRID and
        // must reach the requester, not the app link. A locally generated management reply
        // (our own id_src) or a FLAG_SUCCESS/FAIL ack stays with the app.
        if (IsResponse && (Frame.Flags & (FLAG_SUCCESS | FLAG_FAIL)) == 0 &&
            Frame.Trid >= TRID_SUB_BASE && Frame.Trid <= TRID_SUB_MAX &&
            Frame.IdSrc != DeviceStatus.ShortAddress) {
            Subscriptions.Handle(Frame);
            Handled = true;
        }
#endif
#ifdef USE_APP_INTERFACE
        // An App reply echoes the app's TRID (0xF000-0xFFFF); a subscription-management
        // reply echoes a Subscriptions TRID. Both go back over the app link, where the app
        // matches on TRID.
        if (!Handled && IsResponse &&
            (Frame.Trid >= TRID_APP_BASE ||
             (Frame.Trid >= TRID_SUB_BASE && Frame.Trid <= TRID_SUB_MAX))) {
            (void)AppInterface.Send(Frame);
            Handled = true;
        }
#endif
#ifdef USE_SCRIPTS
        // Script-instance replies (foreign register access) echo the script's TRID
        // (0x2000-0xEFFF), so they route by range instead of by ServiceType.
        if (!Handled && IsResponse &&
            Frame.Trid >= TRID_SCRIPT_BASE && Frame.Trid <= TRID_SCRIPT_MAX) {
            Script.HandleResponse(Frame);
            Handled = true;
        }
#endif
        if (!Handled) switch (TargetSrv)
        {
        case ServiceType::Device:
            DeviceSvc.Handle(Frame);
            break;

        case ServiceType::LogHandler:
            LogHandler.Handle(Frame);
            break;

        case ServiceType::Register:
            Register.Handle(Frame);
            break;

        case ServiceType::Storage:
            StorageSvc.Handle(Frame);
            break;

        case ServiceType::Subscriptions:
#if defined(USE_SUB_PROVIDE) || defined(USE_SUB_REQUEST)
            DeviceLog("DISP", "dispatch Subscriptions cid=%d", Cid);
            Subscriptions.Handle(Frame);
#endif
            break;

        case ServiceType::Script:
#ifdef USE_SCRIPTS
            DeviceLog("DISP", "dispatch Script cid=%d", Cid);
            Script.Handle(Frame);
#endif
            break;

        default:
            DeviceLog("DISP", "unhandled service %u CID %u", (unsigned)TargetSrv, (unsigned)Cid);
            // An unhandled service is an error, so it goes out at the error class.
            ReportLog(MakeLog(false, (uint16_t)TargetSrv, Cid, 0), PRIORITY_ERROR);
            break;
        }
    } // end if (local interest)

    // Forwarding Rule: Always forward to physical bus if the target is external and it
    // originated locally. (The app link's frames are rewritten to our own short address
    // by AppInterfaceRouteIn, so they forward through the same path.)
    if (NetQualifyLocal(Frame.IdTgt, DeviceStatus.NetId) != DeviceStatus.ShortAddress &&
        Frame.IdSrc == DeviceStatus.ShortAddress)
    {
        // One bus transaction at a time, whoever started it: SendAndVerifyPacket asks the gate
        // before it puts the frame on the wire (AppInterface.h AppBusGateOffer), so an app forward
        // and the core's own request enter the medium through the same door. A frame that would
        // collide with an answer still owed is held there and sent by AppBusDeferredDrain when
        // that answer lands - the gate's scope is the wire, not the frame's origin, so nothing
        // has to know here whether this frame came from the app.
        //
        // A false return means the frame never got onto the wire (a gate-held frame reports true and
        // is sent later by AppBusDeferredDrain). For a frame the CORE originated - a subscription
        // install, a script's foreign register access, a confirm-required value update - nothing
        // else records the loss: the gate ring tracks app forwards only, so the request simply never
        // existed while its caller believes it was sent. Counted beside AppBusSendFails.
        const bool Sent = SendAndVerifyPacket(Frame);
#ifdef USE_APP_INTERFACE
        if (!Sent && AppFrameSource == AppLinkSource::None)
            AppBusCoreRequestLost = AppBusCoreRequestLost + 1;
#else
        (void)Sent;
#endif
    }
}

#ifdef TYPE_CORE
// Sends the frames the core held back while the medium was owed an answer (AppInterface.h
// AppBusDefer). Called from the app pump (AppInterfaceService::Pump) every pass, which runs on the
// core's main task whether or not an app is attached.
//
// One answered frame at a time: a frame that opens the transaction slot (BusGateNeedsAnswer)
// closes the loop, because the condition is re-tested before the next frame goes out. So the run
// a closed window releases is never a burst of answered unicasts - exactly the shape the gate
// exists to stop - and only frames that expect NO answer (a value update, a log record) can leave
// back to back, which collide with nothing.
inline void AppBusDeferredDrain()
{
    while (AppBusDeferredCount > 0 && !AppBusForwardPending())
    {
        PacketFrame Frame = AppBusDeferred[0];
        for (uint8_t i = 1; i < AppBusDeferredCount; i++)
            AppBusDeferred[i - 1] = AppBusDeferred[i];
        AppBusDeferredCount--;
        // Dispatched with AppFrameSource None (a core-origin frame): it forwards through the same
        // gate as anything else, so if it expects an answer it opens the transaction slot here and
        // the loop above stops until that answer lands.
        DispatchPacket(Frame);
    }
}
#endif

// Processes the bus input queue, dispatching one received frame per call to the local
// handlers (the main loop calls this repeatedly to drain the queue).
void ProcessBus()
{
    static PacketFrame RxFrame __attribute__((aligned(4)));
    int FrameSize = ReceivePacket(&RxFrame);
    if (FrameSize == 0)
    {
        return;
    }
    DispatchPacket(RxFrame);
}

// Restores every memory service from its backup file at boot (the static `.SV` space, and
// dynamic blocks when compiled in). Not core-only: nodes with Static Memory (e.g. the DAS
// restores Meas1/Meas2) reuse the same path instead of duplicating it per device.
void LoadAllBackups()
{
    // Static memory: one 1:1 mirror read of `.SV` (the whole persistent space).
    Register.StaticRecallAll();

#ifdef USE_DYNAMIC_BLOCKS
    // Per-block DT/DV files: enumerate them once (absent file = tombstone/empty). Probing every
    // slot instead costs a file-table lookup per slot - the same pattern 274a04d fixed for the
    // Recall command; the boot path was left on the probe and took seconds.
    Register.DynamicRecallAll();
#endif

#ifdef USE_SUB_REQUEST
    Subscriptions.LoadRequesterTable();
#endif
}

