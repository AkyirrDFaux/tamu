// Host-side test for the bus transaction gate's DECISIONS (Core/Functions/BusGate.h).
//
// Card t_e5f46574. The gate used to cover only the app's forwarded frames, was asked in the
// dispatcher (so a core-origin request - a subscription poll, a script's register access - went
// onto the wire ungated: two of them could sit 3-10 ms apart with the first answer still owed),
// and it held the WHOLE inbound app queue behind them. The decisions that fix that are pure, and
// they run here against the same header the device uses:
//
//   BusGateNeedsAnswer   which frames are transactions at all (item 1)
//   BusGateWindowMs      the window per request kind (item 4)
//   BusGateHeld          the slot's expiry, wrap-safe
//   BusGateVerdictFor    send / hold / send-and-count-a-clash
//   BusGateQueuePick     which queued frame the pump may dispatch (item 2, head-of-line)
//
// The scenario blocks below compose those real helpers the way the device does (the model of
// AppInterface::AppBusGateOffer is six lines and is repeated at the top of GateSim), so the two
// behaviours the card asks for are asserted as sequences: no two answered unicasts inside one
// window - core-origin or app - and a burst that answers 20/20 per node instead of the measured
// 4/20 of the ungated case.

#include <cstdint>
#include <cstdio>
#include <cstring>

// Packet.h refers to the device status the device TUs own (NetId.h says the same). The gate does
// not read it, so a bare instance is enough to make the header self-contained on the host.
#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Packet.h"
DeviceStatusStruct DeviceStatus;

#include "Core/Functions/BusGate.h"

#include "check.h"

// The measured answers these windows are fitted to (rig 2026-11-06, .pio/probe/gate_latency.py and
// restore_measure4.gate.log): an ordinary forwarded read/write 13-14 ms, the longest answered
// forward of a whole restore 171 ms, a healthy node's Save All 125-184 ms.
static const uint16_t MeasuredReadMs = 14;
static const uint16_t MeasuredWorstForwardMs = 171;
static const uint16_t MeasuredSaveAllMs = 184;

// Frame shapes, as the real senders build them.
static const uint8_t AppRequest = FLAG_REQACK | FLAG_START | FLAG_STOP;             // app request
static const uint8_t CoreRequest = FLAG_REQACK | FLAG_START | FLAG_STOP;            // same flags
static const uint8_t Reply = FLAG_TYPE | FLAG_START | FLAG_STOP;                    // a node's answer
static const uint8_t StatusReply = FLAG_TYPE | FLAG_REQACK | FLAG_START | FLAG_STOP | FLAG_SUCCESS;
static const uint8_t ConfirmUpdate = FLAG_TYPE | FLAG_REQACK | FLAG_START | FLAG_STOP;
static const uint8_t AckNoReply = FLAG_START | FLAG_STOP;                           // one-way ack
// The 040x/1 Change subscription exactly as the tree builds it: no REQACK (SubscriptionsPersist.h
// RegisterRequesterProvider, SubscriptionsControl.h SubscriptionsCancelProvider,
// SubscriptionsRequester.h orphan cancel) although the provider answers it.
static const uint8_t SubChangeRequest = FLAG_START | FLAG_STOP;

static const uint16_t Node02 = MakeId(1, 2);
static const uint16_t Node04 = MakeId(1, 4);
static const uint16_t ReadCmd = MakeService(ServiceType::Register, 1);   // Register CID 1 read
static const uint16_t SaveAllCmd = MakeService(ServiceType::Register, BUS_GATE_SAVE_ALL_CID);
static const uint16_t RecallAllCmd = MakeService(ServiceType::Register, 4);
static const uint16_t StorageCmd = MakeService(ServiceType::Storage, BUS_GATE_SAVE_ALL_CID);
static const uint16_t SubChangeCmd = MakeService(ServiceType::Subscriptions, BUS_GATE_SUB_CHANGE_CID);
static const uint16_t SubValueCmd = MakeService(ServiceType::Subscriptions, 0); // provider value update
static const uint16_t SubCancelCmd = SubChangeCmd;                               // 040x/1, empty payload

// ---- A model of the device half, built ONLY from the real decisions -------------------------
//
// AppInterface::AppBusGateOffer is: BusGateVerdictFor(pending, ring has room) -> hold, or note a
// clash and go on; then BusGateNeedsAnswer -> AppBusForwardStart(BusGateWindowMs(...)).
// AppInterface::AppBusForwardPending is BusGateHeld. AppInterface::Pump picks with
// BusGateQueuePick. Nothing here re-derives a window, an expiry or an ordering rule.
struct GateSim
{
    bool Open = false;
    uint32_t OpenedAt = 0;
    uint16_t Window = 0;
    uint16_t Trid = 0;
    uint16_t Cmd = 0; // the open request's CMD: the answer must echo it (BusGateIsAnswer)
    uint32_t Now = 0;
    uint16_t WireTrid[256] = {};
    int Wire = 0, Deferred = 0, Clashes = 0, Expires = 0, Released = 0;
    int InFlight = 0, MaxInFlight = 0; // transactions on the wire at once: must never exceed 1

    bool Pending()
    {
        if (!Open) return false;
        if (!BusGateHeld(Now, OpenedAt, Window))
        {
            Open = false;
            Expires++;
            InFlight = 0; // the expiry released the slot: whatever was owed is lost, not on the wire
        }
        return Open;
    }

    // One frame offered to the bus. Returns true when it went out now, false when it was held.
    bool Offer(uint16_t Tgt, uint8_t Flags, uint16_t Cmd_, uint16_t Trid_, bool CoreOrigin, bool DeferRoom)
    {
        const bool GateOpen = Pending();
        const BusGateVerdict V = BusGateVerdictFor(GateOpen, DeferRoom);
        if (V == BusGateVerdict::Defer)
        {
            Deferred++;
            return false;
        }
        if (V == BusGateVerdict::SendAndClash)
            Clashes++;
        // A needed answer opens the slot (SendAndVerifyPacket asks this before transmitting).
        if (BusGateNeedsAnswer(Tgt, Flags, Cmd_))
        {
            Open = true;
            OpenedAt = Now;
            Window = BusGateWindowMs(Cmd_, CoreOrigin);
            Trid = Trid_;
            Cmd = Cmd_;
            InFlight++;
            if (InFlight > MaxInFlight) MaxInFlight = InFlight;
        }
        if (Wire < 256) WireTrid[Wire] = Trid_;
        Wire++;
        return true;
    }

    // A frame arrived from another device (Dispatcher -> AppBusForwardSawReply): the answer to the
    // open transaction iff it echoes both the TRID and the CMD.
    void Answer(uint16_t Trid_, uint16_t Cmd_)
    {
        if (Open && BusGateIsAnswer(Trid, Cmd, Trid_, Cmd_))
        {
            Open = false;
            Released++;
            InFlight = 0;
        }
    }
    void Answer(uint16_t Trid_) { Answer(Trid_, Cmd); } // the ordinary case: the same CMD
};

int main()
{
    // ---------------------------------------------------------------------------------------
    // Item 1: which frames are transactions. The gate's scope is the wire, not the origin: a
    // core-origin request that carries REQACK takes the same slot as an app frame.
    // ---------------------------------------------------------------------------------------
    CHECK(BusGateNeedsAnswer(Node02, AppRequest, ReadCmd));          // an app forward
    CHECK(BusGateNeedsAnswer(Node02, CoreRequest, ReadCmd));         // a script access / time sync
    CHECK(BusGateNeedsAnswer(Node02, ConfirmUpdate, SubValueCmd));   // value update awaiting a confirmation
    CHECK(!BusGateNeedsAnswer(ADDR_BROADCAST, AppRequest, ReadCmd)); // several answers: nothing to wait for
    CHECK(!BusGateNeedsAnswer(Node02, StatusReply, ReadCmd));        // a status reply is an answer itself
    CHECK(!BusGateNeedsAnswer(Node02, Reply, ReadCmd));              // a plain answer is nobody's request
    CHECK(!BusGateNeedsAnswer(Node02, AckNoReply, ReadCmd));         // a one-way ack expects nothing

    // The 040x/1 Change subscription: answered WITHOUT carrying REQACK, and the flag-only test -
    // the one the first review round found - missed it entirely. The flags are the tree's own
    // (SubChangeRequest = FLAG_START|FLAG_STOP), so this asserts the frame that is really sent.
    CHECK(BusGateNeedsAnswer(Node02, SubChangeRequest, SubChangeCmd));      // install / renewal
    CHECK(BusGateNeedsAnswer(Node02, SubChangeRequest, SubCancelCmd));      // cancel (FLAG_SUCCESS reply)
    CHECK(BusGateNeedsAnswer(Node02, SubChangeRequest | FLAG_REQACK, SubChangeCmd)); // if the bit is set
    // Only the CID makes it one: an answer-less unicast of any other kind stays ungated, and a
    // BROADCAST Change still opens nothing (several providers may answer).
    CHECK(!BusGateNeedsAnswer(Node02, SubChangeRequest, ReadCmd));
    CHECK(!BusGateNeedsAnswer(Node02, SubChangeRequest, SubValueCmd));
    CHECK(!BusGateNeedsAnswer(Node02, SubChangeRequest, StorageCmd));
    CHECK(!BusGateNeedsAnswer(ADDR_BROADCAST, SubChangeRequest, SubChangeCmd));
    // A status reply is never a transaction, whatever its CID.
    CHECK(!BusGateNeedsAnswer(Node02, SubChangeRequest | FLAG_TYPE | FLAG_SUCCESS, SubChangeCmd));

    // What closes it: the provider's reply to the Change - Cmd echoed (SubReply carries the
    // request's Cmd), the current value for an install/renewal or FLAG_SUCCESS for a cancel.
    const uint16_t SubTrid = TRID_SUB_BASE + 7;
    CHECK(BusGateIsAnswer(SubTrid, SubChangeCmd, SubTrid, SubChangeCmd));
    // ... and what must NOT close it early: the provider's value UPDATE, which echoes the same
    // subscription TRID as CID 0 (SubscriptionsProvider.h), and an unrelated frame.
    CHECK(!BusGateIsAnswer(SubTrid, SubChangeCmd, SubTrid, SubValueCmd));
    CHECK(!BusGateIsAnswer(SubTrid, SubChangeCmd, SubTrid + 1, SubChangeCmd));
    // A confirm-required value update's slot is closed by the confirmation, which carries the
    // Cmd of the update it answers (SubscriptionsRequester.h, CID 0, no FLAG_TYPE).
    CHECK(BusGateIsAnswer(SubTrid, SubValueCmd, SubTrid, SubValueCmd));
    CHECK(!BusGateIsAnswer(SubTrid, SubValueCmd, SubTrid, SubChangeCmd));

    // The old rule, kept here as the defect: gating keyed on the APP TRID range, which is exactly
    // what "AppFrameSource != None" did at the dispatcher. A core request (subscription TRIDs are
    // 0x1000-0x1FFF) was never gated.
    const uint16_t CorePollTrid = TRID_SUB_BASE + 5;
    const uint16_t AppTrid = TRID_APP_BASE + 1;
    CHECK_EQ(BusGateNeedsAnswer(Node02, CoreRequest, ReadCmd) && CorePollTrid < TRID_APP_BASE, 1); // new: gated
    CHECK_EQ(CorePollTrid < TRID_APP_BASE, 1);                                            // old: not gated
    CHECK_EQ(AppTrid >= TRID_APP_BASE, 1);                                                // old: gated

    // ---------------------------------------------------------------------------------------
    // Item 4: the window per request kind, fitted to that kind's slowest legitimate answer.
    // ---------------------------------------------------------------------------------------
    CHECK_EQ(BusGateWindowMs(ReadCmd, false), BUS_GATE_WINDOW_MS);
    CHECK_EQ(BusGateWindowMs(ReadCmd, true), BUS_GATE_WINDOW_CORE_MS);
    CHECK_EQ(BusGateWindowMs(SaveAllCmd, false), BUS_GATE_WINDOW_STORE_MS);
    CHECK_EQ(BusGateWindowMs(SaveAllCmd, true), BUS_GATE_WINDOW_STORE_MS); // the kind wins
    CHECK_EQ(BusGateWindowMs(RecallAllCmd, false), BUS_GATE_WINDOW_MS);    // only Save All is long
    CHECK_EQ(BusGateWindowMs(StorageCmd, false), BUS_GATE_WINDOW_MS);      // another service's CID 5
    // The fits the header states, as numbers.
    CHECK(BUS_GATE_WINDOW_MS >= MeasuredWorstForwardMs * 3);
    CHECK(BUS_GATE_WINDOW_CORE_MS >= MeasuredReadMs * 10);
    CHECK(BUS_GATE_WINDOW_STORE_MS >= MeasuredSaveAllMs * 8);
    CHECK(BUS_GATE_WINDOW_STORE_MS < 25000); // the app's own Save All budget (backup_restore.dart)

    // ---------------------------------------------------------------------------------------
    // The slot's expiry: a subtraction, and the exceptional path (an answer releases it first).
    // ---------------------------------------------------------------------------------------
    CHECK(BusGateHeld(100, 0, 600));
    CHECK(BusGateHeld(599, 0, 600));
    CHECK(!BusGateHeld(600, 0, 600));
    CHECK(!BusGateHeld(601, 0, 600));
    // Free-running uptime: the window must survive the counter wrapping.
    CHECK(BusGateHeld(0x40u, 0xFFFFFF00u, 600));  // 320 ms elapsed across the wrap
    CHECK(!BusGateHeld(0x200u, 0xFFFFFF00u, 600)); // 768 ms elapsed
    CHECK(BusGateHeld(0x200u, 0xFFFFFF00u, 3000)); // a Save All still inside its own window

    // ---------------------------------------------------------------------------------------
    // The admission verdict.
    // ---------------------------------------------------------------------------------------
    CHECK(BusGateVerdictFor(false, true) == BusGateVerdict::SendNow);
    CHECK(BusGateVerdictFor(false, false) == BusGateVerdict::SendNow);
    CHECK(BusGateVerdictFor(true, true) == BusGateVerdict::Defer);
    CHECK(BusGateVerdictFor(true, false) == BusGateVerdict::SendAndClash);

    // ---------------------------------------------------------------------------------------
    // Item 2: the head-of-line block is gone - a frame that needs no bus is dispatched while a
    // forward waits, and the bus frames keep their order among themselves.
    // ---------------------------------------------------------------------------------------
    const bool AllBus[3] = {true, true, true};
    const bool BusLocalBus[3] = {true, false, true};
    const bool LocalFirst[2] = {false, true};
    CHECK_EQ(BusGateQueuePick(AllBus, 3, false), 0);        // nothing owed: in order
    CHECK_EQ(BusGateQueuePick(AllBus, 3, true), -1);        // all wait for the wire
    CHECK_EQ(BusGateQueuePick(BusLocalBus, 3, true), 1);    // the core-local frame goes now
    CHECK_EQ(BusGateQueuePick(BusLocalBus, 3, false), 0);   // in order when the wire is free
    CHECK_EQ(BusGateQueuePick(LocalFirst, 2, true), 0);     // the head needs no bus anyway
    CHECK_EQ(BusGateQueuePick(AllBus, 0, true), -1);        // nothing queued
    CHECK_EQ(BusGateQueuePick(AllBus, 1, true), -1);        // a single blocked frame waits

    // Taking the local frame out past the head leaves the queue's order untouched (the pump shifts
    // the frames before it up one slot): [bus A, local, bus B] -> [bus A, bus B].
    {
        const char Queue[] = {'A', 'L', 'B'};
        char After[3] = {'\0', '\0', '\0'};
        bool NeedsBus[3];
        for (int i = 0; i < 3; i++) NeedsBus[i] = Queue[i] != 'L';
        const int Pick = BusGateQueuePick(NeedsBus, 3, true);
        CHECK_EQ(Pick, 1);
        int Out = 0;
        for (int i = 0; i < 3; i++)
            if (i != Pick) After[Out++] = Queue[i];
        CHECK_EQ(After[0], 'A');
        CHECK_EQ(After[1], 'B');
        CHECK(NeedsBus[0]); // still the head, still a bus frame
    }

    // ---------------------------------------------------------------------------------------
    // Item 1, as a sequence: two core-origin answered requests 3 ms apart. Under the OLD rule -
    // the dispatcher opened the slot only for an app-origin frame (`AppFrameSource != None`), which
    // in practice meant only the app's TRID range - both go out 3 ms apart with the first answer
    // still owed. Under the new one the second is held.
    // ---------------------------------------------------------------------------------------
    {
        // The old rule, modelled exactly: a frame opened the slot only if it came from the app.
        const auto OldRuleNeedsAnswer = [](uint8_t Flags, uint16_t Trid) {
            return BusGateNeedsAnswer(Node02, Flags, ReadCmd) && Trid >= TRID_APP_BASE;
        };
        int OldSent = 0;
        bool OldOpen = false;
        for (int i = 0; i < 2; i++)
        {
            if (!OldOpen)
            {
                OldSent++;
                if (OldRuleNeedsAnswer(CoreRequest, CorePollTrid)) OldOpen = true;
            }
            // else: the dispatcher held it in the RX queue (no gate for a core frame at all)
        }
        CHECK_EQ(OldSent, 2); // the defect: two requests on the wire, one answer owed for both

        GateSim Sim;
        Sim.Now = 1000;
        CHECK(Sim.Offer(Node02, CoreRequest, ReadCmd, CorePollTrid, true, true));
        CHECK(Sim.Pending()); // the slot is now owed an answer
        Sim.Now += 3;         // the second request is built 3 ms later
        const bool Second = Sim.Offer(Node04, CoreRequest, ReadCmd, CorePollTrid + 1, true, true);
        CHECK(!Second);          // held, not sent
        CHECK_EQ(Sim.Wire, 1);   // only one request on the wire
        CHECK_EQ(Sim.Deferred, 1);
        CHECK_EQ(Sim.MaxInFlight, 1);
        // The answer lands, the held frame goes out.
        Sim.Now += 12;
        Sim.Answer(CorePollTrid);
        CHECK_EQ(Sim.Released, 1);
        Sim.Now += 1;
        CHECK(Sim.Offer(Node04, CoreRequest, ReadCmd, CorePollTrid + 1, true, true));
        CHECK_EQ(Sim.Wire, 2);
        CHECK_EQ(Sim.Clashes, 0);
        CHECK_EQ(Sim.MaxInFlight, 1); // never two requests on the wire at once
    }

    // An app forward and a core request share the one slot, in either order.
    {
        GateSim Sim;
        Sim.Now = 500;
        CHECK(Sim.Offer(Node02, AppRequest, ReadCmd, AppTrid, false, true));
        CHECK(!Sim.Offer(Node04, CoreRequest, ReadCmd, CorePollTrid, true, true));
        CHECK_EQ(Sim.Deferred, 1);
        Sim.Now += 14;
        Sim.Answer(AppTrid);
        Sim.Now += 1;
        CHECK(Sim.Offer(Node04, CoreRequest, ReadCmd, CorePollTrid, true, true));
        Sim.Now += 3; // the node answers the core's poll immediately after
        Sim.Answer(CorePollTrid);
        CHECK_EQ(Sim.Released, 2);
        CHECK_EQ(Sim.Expires, 0);
    }

    // ---------------------------------------------------------------------------------------
    // Item 1, the shape the first review round found ungated: two 040x/1 Changes (the second row's
    // attempt at the same instant) 3 ms apart, with no REQACK on either. Under the old flag-only
    // test neither opened a slot; under the CID rule the second is held, and a provider's VALUE
    // UPDATE arriving with the same subscription TRID does NOT close the first one's slot - only
    // the Change's own answer does.
    // ---------------------------------------------------------------------------------------
    {
        const uint16_t SubTrid1 = TRID_SUB_BASE + 3;
        const uint16_t SubTrid2 = TRID_SUB_BASE + 4;

        // The old test, verbatim: REQACK only.
        const auto OldNeedsAnswer = [](uint8_t Flags) { return (Flags & FLAG_REQACK) != 0; };
        CHECK(!OldNeedsAnswer(SubChangeRequest)); // the defect: the answered Change opened nothing

        GateSim Sim;
        Sim.Now = 4000;
        CHECK(Sim.Offer(Node02, SubChangeRequest, SubChangeCmd, SubTrid1, /*CoreOrigin=*/true, true));
        CHECK(Sim.Pending());                       // a Change now holds the medium
        Sim.Now += 3;
        CHECK(!Sim.Offer(Node04, SubChangeRequest, SubChangeCmd, SubTrid2, true, true)); // held
        CHECK_EQ(Sim.Wire, 1);

        // A provider value update for the SAME subscription TRID lands (CID 0, FLAG_TYPE). It is
        // not the answer to the Change: the slot must stay owed, or the next frame goes out on top
        // of the Change's real answer.
        Sim.Now += 2;
        Sim.Answer(SubTrid1, SubValueCmd);
        CHECK_EQ(Sim.Released, 0);
        CHECK(Sim.Pending());
        CHECK(!Sim.Offer(Node04, SubChangeRequest, SubChangeCmd, SubTrid2, true, true)); // still held
        CHECK_EQ(Sim.Wire, 1);

        // The provider's answer to the Change: Cmd echoed, current value (or FLAG_SUCCESS for a
        // cancel). That is what closes the slot, and the held Change then goes out.
        Sim.Now += 9;
        Sim.Answer(SubTrid1, SubChangeCmd);
        CHECK_EQ(Sim.Released, 1);
        Sim.Now += 1;
        CHECK(Sim.Offer(Node04, SubChangeRequest, SubChangeCmd, SubTrid2, true, true));
        CHECK_EQ(Sim.Wire, 2);
        CHECK_EQ(Sim.Clashes, 0);
        CHECK_EQ(Sim.MaxInFlight, 1);
        CHECK_EQ(Sim.Expires, 0);
    }

    // A cancel is the same CID, answered with FLAG_SUCCESS: same slot, same close.
    {
        const uint16_t CancelTrid = TRID_SUB_BASE + 9;
        GateSim Sim;
        Sim.Now = 6000;
        CHECK(Sim.Offer(Node02, SubChangeRequest, SubCancelCmd, CancelTrid, true, true));
        CHECK(Sim.Pending());
        Sim.Now += 6;
        Sim.Answer(CancelTrid, SubCancelCmd);
        CHECK_EQ(Sim.Released, 1);
        CHECK_EQ(Sim.Expires, 0);
    }


    // A frame that expects no answer is still held while an answer is owed (one medium), and the
    // deferral ring's fullness is what turns that into a counted clash rather than a silent loss.
    // This is the rule Dispatcher.h and BusGate.h now both state: the gate holds EVERY frame while an
    // answer is owed - the wire has no idle moment to slip a value update or a log record into - and
    // the holds that cost an answer-less frame its time are counted (AppBusDeferredNoAnswer) rather
    // than being invisible.
    {
        GateSim Sim;
        Sim.Now = 700;
        CHECK(Sim.Offer(Node02, AppRequest, ReadCmd, AppTrid, false, true));
        CHECK(!BusGateNeedsAnswer(Node04, Reply, ReadCmd));           // nothing gated it by answering
        CHECK(!Sim.Offer(Node04, Reply, ReadCmd, 0x0101, true, true)); // held anyway
        CHECK_EQ(Sim.Deferred, 1);
        CHECK(Sim.Offer(Node04, Reply, ReadCmd, 0x0101, true, false)); // ring full: it leaves
        CHECK_EQ(Sim.Clashes, 1);
    }

    // ---------------------------------------------------------------------------------------
    // The card's second find: the SAME app request keeps the same window whether it went straight
    // out or had to wait in the deferral ring. The drain dispatches a held frame with the link it
    // arrived on (Dispatcher.h restores AppBusDeferredFrom), so what classifies the transaction is
    // the frame's origin - not "was the app pump calling right now".
    // ---------------------------------------------------------------------------------------
    {
        const uint16_t StorageRead = MakeService(ServiceType::Storage, 1);

        // The two deadlines the review measured against the real headers: an app forward 600 ms, the
        // same frame re-sent as a core-origin request 250 ms.
        CHECK_EQ(BusGateWindowMs(StorageRead, /*CoreOrigin=*/false), BUS_GATE_WINDOW_MS);
        CHECK_EQ(BusGateWindowMs(StorageRead, /*CoreOrigin=*/true), BUS_GATE_WINDOW_CORE_MS);
        CHECK_EQ(BUS_GATE_WINDOW_MS, 600);
        CHECK_EQ(BUS_GATE_WINDOW_CORE_MS, 250);

        // A node answering at 300 ms is inside the window fitted for a forwarded read (BusGate.h) and
        // outside the core's. Forwarded, that is a healthy answer.
        GateSim Sim;
        Sim.Now = 100;
        CHECK(Sim.Offer(Node02, AppRequest, StorageRead, AppTrid, /*CoreOrigin=*/false, true));
        Sim.Now += 300;
        CHECK(Sim.Pending()); // 300 ms into a forwarded read: still owed, not expired
        Sim.Answer(AppTrid);
        CHECK_EQ(Sim.Released, 1);
        CHECK_EQ(Sim.Expires, 0);

        // The same 300 ms in the core's window expires - the deadline a QUEUED app forward used to be
        // given: the next frame goes out on top of the answer and the app reports a timeout for a node
        // that answered.
        GateSim Core;
        Core.Now = 100;
        CHECK(Core.Offer(Node02, CoreRequest, StorageRead, CorePollTrid, /*CoreOrigin=*/true, true));
        Core.Now += 300;
        CHECK(!Core.Pending());
        CHECK_EQ(Core.Expires, 1);
    }

    // ---------------------------------------------------------------------------------------
    // Item 4, as a sequence: a Save All legitimately runs long, and the frames behind it wait for
    // ITS window - not for a read's.
    // ---------------------------------------------------------------------------------------
    {
        GateSim Sim;
        Sim.Now = 10;
        CHECK(Sim.Offer(Node04, AppRequest, SaveAllCmd, AppTrid, false, true));
        Sim.Now += MeasuredSaveAllMs; // a healthy save
        CHECK(Sim.Pending());         // still within its own window (a read's 600 would pass)
        Sim.Now = 10 + 1000;          // the reclaim path: slower than any read ever measured
        CHECK(Sim.Pending());
        Sim.Now = 10 + BUS_GATE_WINDOW_STORE_MS - 1;
        CHECK(Sim.Pending());
        Sim.Now = 10 + BUS_GATE_WINDOW_STORE_MS;
        CHECK(!Sim.Pending()); // expiry is the exceptional path: release it first
        CHECK_EQ(Sim.Expires, 1);

        // Released on the answer, and the read behind it then keeps ITS shorter window.
        GateSim Sim2;
        Sim2.Now = 10;
        CHECK(Sim2.Offer(Node04, AppRequest, SaveAllCmd, AppTrid, false, true));
        Sim2.Now += 1200;
        Sim2.Answer(AppTrid);
        CHECK_EQ(Sim2.Released, 1);
        CHECK_EQ(Sim2.Expires, 0);
        Sim2.Now += 1;
        CHECK(Sim2.Offer(Node02, AppRequest, ReadCmd, AppTrid + 1, false, true));
        Sim2.Now += 13;
        CHECK(Sim2.Pending());
        Sim2.Now += (BUS_GATE_WINDOW_MS - 13);
        CHECK(!Sim2.Pending()); // a read's window, not the save's
    }

    // ---------------------------------------------------------------------------------------
    // The loaded burst (item 5), modelled on the host: 20 identity reads per node, four nodes, the
    // answers arriving 13-14 ms later as they did on the rig. The ungated shape answered 4/20
    // (measurement in AppInterface.h); the gated one must answer every read.
    // ---------------------------------------------------------------------------------------
    {
        GateSim Sim;
        Sim.Now = 2000;
        const uint16_t Nodes[4] = {MakeId(1, 2), MakeId(1, 3), MakeId(1, 4), MakeId(1, 5)};
        int Answered = 0;
        uint16_t Trid = TRID_APP_BASE;
        // Every node has 20 reads queued; the app fires them as fast as the gate lets it, and each
        // node answers 13 ms after ITS request reached the wire.
        int Remaining[4] = {20, 20, 20, 20};
        uint32_t AnswerAt = 0;
        uint16_t InFlightTrid = 0;
        bool Busy = false;
        while (Remaining[0] + Remaining[1] + Remaining[2] + Remaining[3] > 0 || Busy)
        {
            Sim.Now++;
            if (Busy && Sim.Now >= AnswerAt)
            {
                Sim.Answer(InFlightTrid);
                Answered++;
                Busy = false;
            }
            if (Busy || Sim.Pending()) continue; // the medium is owed an answer: nothing goes out
            for (int n = 0; n < 4 && !Busy; n++)
            {
                if (Remaining[n] == 0) continue;
                Trid++;
                if (!Sim.Offer(Nodes[n], AppRequest, ReadCmd, Trid, false, true))
                {
                    // Held for the frame ahead of it: it is offered again on a later pass.
                    continue;
                }
                Remaining[n]--;
                Busy = true;
                InFlightTrid = Trid;
                AnswerAt = Sim.Now + 13; // measured: an ordinary read answers in 13-14 ms
            }
        }
        CHECK_EQ(Answered, 80);      // 20 per node, 4 nodes
        CHECK_EQ(Sim.Wire, 80);      // and no request went out twice
        CHECK_EQ(Sim.MaxInFlight, 1); // exactly one transaction on the wire at any moment
        CHECK_EQ(Sim.Expires, 0);    // nothing was cut off by a window
        CHECK_EQ(Sim.Clashes, 0);
    }

    printf("bus_gate: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
