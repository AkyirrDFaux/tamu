#pragma once

// BLE link for the App Interface: Nordic UART style GATT service (same UUIDs as the
// previous Chirp implementation), notify (device -> app) + write (app -> device).
// Each transfer carries a uint16 LE length prefix followed by that many packet-stream
// bytes (Docs/Services/App Interface.md BLE packet layout).
//
// Requires AppUSB.h to be included first (shares WireStreamParser).

#include "NimBLEDevice.h"
#include "Blocks/BleAdmissionPolicy.h"
#include "Blocks/BleLinkLatch.h"     // the single-session latch and its supervisor
#include "Blocks/BleRxAssembler.h"   // the bounded, self-resynchronising chunk assembler
#include "Blocks/BleSecurityPolicy.h"
#include "Blocks/BleWindowPolicy.h"
#include "BlePasskeyStore.h" // the passkey and the policy stamp of the bond store (NVS `ble`)

#define BLE_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_RX_CHAR_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // app writes here
#define BLE_TX_CHAR_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // device notifies

#define BLE_CHUNK 480          // stream bytes per notification (+2 byte length prefix);
                               // the actual chunk cap is BleMtu - 5 (ATT header + length
                               // prefix), so 480 is only the static buffer size

static NimBLEServer *BleServer = nullptr;
static NimBLECharacteristic *BleTx = nullptr;
static uint32_t NotifyFails = 0;   // consecutive failed notify() calls

static bool LoggedFirstWrite = false;

void AppbleResetSessionBreadcrumbs()
{
    NotifyFails = 0;
    LoggedFirstWrite = false;
}
static volatile bool BleConnected = false;
static bool BleOldConnected = false;   // for deferred advertising restart
static bool BleAdvRestart = false;     // advertising must be restarted
static volatile uint16_t BleMtu = 23;  // negotiated ATT MTU (ATT default)

// ===== Link security =====
//
// Security is enforced at the Bluetooth level only: the characteristics' flags demand an
// encrypted (and, with a passkey, authenticated) link, so the ATT server refuses an unsecured
// peer before any application code runs, and the app session below is not established until the
// link reports encrypted+bonded. There is no application-layer authentication on top.
//
// The policy itself is NOT stored here: it is derived from the `BLE Settings` block's Passkey
// field on every use (BlePolicyNow), so the register field is the single source of truth. The
// same block owns the radio's enable state and the advertised name (AppBLESettingsApply).
#define BLE_WINDOW_MS 60000           // the pairing window a host usually asks for (a write sets its own)
#define BLE_AUTO_WINDOW_MS 300000     // ... and opened by itself for 5 min while no bond is stored
// A link that has not completed pairing by its deadline is dropped (AppBLETick). The budget itself
// is policy-shaped and lives with the policy (Blocks/BleSecurityPolicy.h, BleSecureBudgetMs): Just
// Works waits for one tap, a passkey waits for a person to read a prompt and type six digits. Both
// fit inside the connectable window the pairing was started in, which is why the passkey figure has
// to stay below BLE_WINDOW_MS - pinned here so the two numbers cannot drift apart again.
static_assert(BleSecureBudgetPasskeyMs < BLE_WINDOW_MS,
              "a passkey pairing must fit inside the window that opened it");

static volatile bool BleSecured = false;      // current link encrypted, bonded (and authenticated)
// The session flag as a question: a link is a SESSION only when the security procedure completed
// under the policy in force (onAuthenticationComplete) and the session gate admitted it. Cleared on
// disconnect and whenever the radio is taken down. This is what the `BLE Settings` block's write
// rule asks (Blocks/BleWritePolicy.h): a BLE frame may write the block only on such a link, never
// merely because it is not the cable.
bool AppBLELinkSecured()
{
    return BleSecured && BleConnected;
}

static uint32_t BleWindowEndMs = 0;           // 0 = the connectable window is closed
// The pairing deadline: a link that has not become a session by then is dropped by AppBLETick. It is
// armed for EVERY link that is left up without a session - including the refusals, which used to
// return before a deadline existed and so could leave a link with no timeout at all - and it is
// cleared only once the drop has been handed to the latch's supervisor (Blocks/BleLinkLatch.h), which
// is what retries it and clears the latch if the stack will not co-operate.
static uint16_t BleSecureHandle = 0xFFFF;     // the link the deadline belongs to
static uint32_t BleSecureDeadlineMs = 0;      // 0 = no unadmitted link pending
static uint32_t BleSecureBudgetUsedMs = 0;    // the budget the pending link was granted (for the log)
static bool BleSecureLinkMitm = false;        // ... and whether it was granted the passkey budget

// The one link that may hold the radio, and the supervisor that makes its release verifiable.
// The session handle used to be a bare variable: set where a link arrived, cleared only in
// onDisconnect, and every refusal's `disconnect()` answer was thrown away - so a teardown that did
// not produce the callback left the device advertising and dropping every peer for the life of the
// boot (Blocks/BleLinkLatch.h has the whole story). The state machine owns it now, and AppBLETick
// checks it against the stack's own list of live links instead of trusting the callback.
static BleLinkLatch StaticBleLatch;

// When the current link came up (uptime ms, taken in onConnect). The connect-time refusal is only
// "immediate" if it can be measured, so onDisconnect logs the link's whole lifetime: a refused link
// reads a few milliseconds, a session reads as long as the app was connected (Issues.md E73).
static uint32_t BleLinkUpMs = 0;
static bool BleAdvStateApplied = false;       // the advertisement matches BleAdvWanted()
// The bond store as it was when the current link came up, and whether the peer that finished the
// security procedure is one of those bonds. The session gate asks the second question (E66;
// Blocks/BleAdmissionPolicy.h), so the answer has to be taken before the link can change the store.
static NimBLEAddress BleLinkBonds[CONFIG_BT_NIMBLE_MAX_BONDS];
static uint8_t BleLinkBondCount = 0;
// True while the current link may not create a bond: no window is open and the store is not empty,
// so a pairing on this link is a stranger's and must not leave a bond behind.
static bool BleLinkBondingClosed = false;
// Owed to a link that made the device's FIRST bond: the naming rule's write (Blocks/AutoName.h,
// the trigger) is raised from the security callback - where `BleLinkBondCount == 0` and the peer is
// now bonded, i.e. exactly the session gate's empty-store case - and performed by the app task on
// its next pass (Devices/Tamu_v2.0A/BleAutoName.h). Host task writes it, app task consumes it.
static volatile bool BleAutoNameOwed = false;
// The automatic window's trigger: an EVENT the owner caused, consumed by the next AppBLETick pass.
// Never "the bond store reads empty" - a deletion a LINK drove (repeat pairing, E69) empties the
// store too, and that must not make a locked device offer itself. Blocks/BleWindowPolicy.h has the
// rule, and the only two events are the radio starting with no bond (headless first use) and an
// explicit forget emptying the store (zeroed `Bonds`, a passkey change, the epoch revoke - all three
// run through AppBLEForgetAllBonds). App task only: written by the register/USB path and by the
// radio start, read by the tick in the same task.
static BleWindowCause BleAutoWindowCause = BleWindowCauseNone;

// Radio state, straight from the block's Enabled/Display name fields. `BleRadioUp` means the
// NimBLE host and the GATT table exist; `BleEnabled` means the block wants the radio usable. A
// device booted with Enabled = false never initialises NimBLE at all - no radio, no host task.
static bool BleRadioUp = false;
static volatile bool BleEnabled = false;
static char BleFallbackName[SYSTEM_NAME_LEN + 1] = {0}; // System.Name, used when Display name is empty
static char BleAppliedName[SYSTEM_NAME_LEN + 1] = {0};  // the name the advertisement carries now

// The passkey text -> the whole radio+attribute policy. The text is the block's live Passkey
// field, which the NVS-backed store loads at boot and every accepted write updates, so the
// register field is the single source of truth and there is no cached policy here. Cheap (6
// bytes), so it is re-derived instead of cached: a written passkey is in force immediately.
static BleSecurityPolicy BlePolicyNow()
{
    return BleSecurityPolicyFrom(StaticVol.BleSet.Passkey);
}

// The advertised/device name: the block's Display name when it has one, else System.Name. The
// Display name field is a fixed-size character field (null-padded), so trailing NULs and spaces
// are trimmed; a field of nothing but padding means "not set".
static void BleEffectiveName(char *Out)
{
    const char *Display = StaticPer.BleSet.DisplayName;
    uint8_t Len = BLE_DISPLAY_NAME_LEN;
    while (Len > 0 && (Display[Len - 1] == '\0' || Display[Len - 1] == ' '))
        Len--;
    if (Len > 0 && Display[0] != '\0')
    {
        memcpy(Out, Display, Len);
        Out[Len] = '\0';
        return;
    }
    memcpy(Out, BleFallbackName, SYSTEM_NAME_LEN + 1);
}

// The GAP Device Name characteristic (0x2A00): the string a peer's OS reads once it is connected,
// and the name an already-bonded laptop or phone keeps showing while the advertisement's name is
// only what a scan sees. NimBLE initialises it from CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME, which is
// "nimble" - and the part that is not obvious: the server's FIRST start throws away whatever name
// `NimBLEDevice::init()` was given, because `NimBLEServer::start()` calls `resetGATT()`, which calls
// `ble_svc_gap_init()` and re-initialises the GAP service name from that same config value (it is
// the only `ble_svc_gap_init()` call in the image). So a set before the first advertisement is lost,
// and the name has to be applied again once the server exists: StartAppAdvertising does that on
// every (re)build, and the rename path does it for a rename that cannot re-advertise yet. One
// helper, so the advertisement and the characteristic can never be set from different strings.
//
// Length: no truncation is ever applied, because the name cannot get near any limit on this path.
// Both sources are fixed 16-byte fields - `System.Name` (SYSTEM_NAME_LEN, Core/Services/
// StaticMemory.h) and the block's Display name (BLE_DISPLAY_NAME_LEN, Blocks/BleSettings.h) - so
// the effective name is at most 16 characters: inside the 31-byte advertising/scan-response packet
// (16 + the 2-byte AD length/type header), inside NimBLE's own GAP-name cap
// (CONFIG_BT_NIMBLE_GAP_DEVICE_NAME_MAX_LEN=31, sdkconfig.Tamu_v2_0A:1142) and far inside the
// 248-byte the GATT Device Name characteristic allows. `setDeviceName` REFUSES an over-long name
// rather than shortening it (returns false, logged here), and `Adv->setName` returns false the same
// way, so if a future wider field ever exceeded a limit the two would fail loudly instead of
// silently carrying different strings. 0x2A00 therefore always holds the full name the
// advertisement shows.
static bool BleSetGapName(const char *Name)
{
    const bool Ok = NimBLEDevice::setDeviceName(Name);
    if (!Ok)
        DeviceLog("APPBLE", "GAP device name (0x2A00) refused, it stays as it was: '%s'", Name);
    return Ok;
}



// Fed and reset only from the NimBLE host task (BleTxCallbacks::onWrite and the
// connect/disconnect callbacks), never from AppBLETick - see the framer race note there.
static WireStreamParser BleParser;

// Reassembles the length-prefixed BLE chunks (uint16 LE + stream). BlueZ may deliver
// characteristic writes fragmented or coalesced arbitrarily, so this cannot assume one onWrite ==
// one complete chunk. The assembler itself is Blocks/BleRxAssembler.h: a declared length beyond
// what the link can carry is refused, a chunk that stops arriving is abandoned on a clock, and both
// are counted (`Resyncs`) so a desync says so instead of quietly failing every CRC for the rest of
// the session. Its `MaxNeed` is the negotiated MTU's bound, tightened in onMTUChange.
static BleRxAssembler StaticBleRxAssembler;

// ===== The session latch's helpers =====
//
// The rules live in Blocks/BleLinkLatch.h; these are the only places the stack's connection list and
// the disconnect call are touched, so the rules are about state and never about a nil check.

// How often the app task asks the stack whether the latched link is still there. Once per half
// second is far faster than any teardown that matters and costs a vector read.
#define BLE_LATCH_CHECK_MS 500

// A refused link gets this long to tear down before the tick drops it. Shorter than a pairing
// budget on purpose: nothing is waiting for a refusal to complete.
#define BLE_REFUSAL_GRACE_MS 2000

// Is `Handle` one of the stack's live connections? This is what the latch is checked against -
// NimBLEServer's own connection table, not our flags - and it is what lets a lost onDisconnect be
// repaired instead of becoming permanent.
//
// Read with the host's own `ble_gap_conn_find`, which copies a descriptor for one handle: no
// allocation and nothing to invalidate. It is deliberately NOT `NimBLEServer::getPeerDevices()` -
// that accessor builds a fresh `std::vector` out of the server's fixed array on every call
// (managed_components/h2zero__esp-nimble-cpp/src/NimBLEServer.cpp:388), i.e. one heap allocation per
// check, and this is asked from the app task every 500 ms and from `onConnect` in the host task. An
// allocation that failed there read as "not listed", and "not listed" is the heal: a healthy
// encrypted session torn down, advertising restarted, a second phone admitted into the freed latch -
// two peers into one RX stream, which is the one thing the latch exists to prevent. Here "not found"
// is an answer (BLE_HS_ENOTCONN), never a failure, so a link is only ever reported gone when the
// stack says it is.
static bool BleHandleLive(uint16_t Handle)
{
    if (Handle == 0xFFFF || !BleServer)
        return false; // no handle, or no stack at all: there is no connection to be listed
    struct ble_gap_conn_desc Desc;
    return ble_gap_conn_find(Handle, &Desc) == 0;
}

// Asks the stack to drop `Handle` and RECORDS the request, so the tick can tell a teardown that
// happened from one the stack refused. Every refusal path and every deadline goes through here;
// before, each one called disconnect() and threw the answer away.
static void BleDropLink(uint16_t Handle, const char *Why)
{
    if (Handle == 0xFFFF)
        return;
    BleLatchDrop(StaticBleLatch, Handle, TimeFromBoot());
    if (!BleServer)
        return;
    if (BleServer->disconnect(Handle))
        DeviceLog("APPBLE", "drop requested: handle %u (%s)", (unsigned)Handle, Why);
    else
        DeviceLog("APPBLE", "drop of handle %u refused by the stack (%s): the supervisor retries it",
                  (unsigned)Handle, Why);
}

// The pairing deadline for `Handle`: the link has `BudgetMs` to become a session before the tick
// drops it. Armed on every path that leaves a link up WITHOUT a session - the judged path, and now
// the refusals too (they used to return before a deadline existed, so a refused link that would not
// tear down had no timeout at all).
static void BleArmSecureDeadline(uint16_t Handle, uint32_t BudgetMs, bool Mitm)
{
    BleSecureHandle       = Handle;
    BleSecureBudgetUsedMs = BudgetMs;
    BleSecureLinkMitm     = Mitm;
    BleSecureDeadlineMs   = TimeFromBoot() + BudgetMs;
}

static void BleClearSecureDeadline()
{
    BleSecureHandle     = 0xFFFF;
    BleSecureDeadlineMs = 0;
}

// Everything a session owns, released. onDisconnect and the latch's self-heal both end a session
// through this, so a healed latch cannot leave a session flag set on a link that no longer exists -
// which is what would keep advertising off (`BleConnected`) with nothing on the air to connect to.
//
// `WasSession` is passed in rather than re-derived from the latch: the supervisor's heal comes back
// from BleLatchTick AFTER the tick has cleared the latch, so asking the latch again there always
// answered "no" and the teardown did nothing. Every caller knows the answer - onDisconnect before the
// release, the supervisor from the session it captured before the tick.
//
// A link that was never the session (a refused second peer) ending changes nothing about the live
// session: only its own record goes. Without that distinction a second peer's disconnect cleared the
// session's flags while the session was still up, which stopped its TX and put the advertisement back
// on the air underneath it.
static void BleSessionEnded(uint16_t Handle, bool WasSession)
{
    BleLatchReleased(StaticBleLatch, Handle);
    if (BleSecureHandle == Handle)
        BleClearSecureDeadline();
    if (!WasSession)
        return;

    BleConnected = false;
    BleSecured   = false;
    StaticBleRxAssembler.Reset();
    BleParser.Reset();
    BleMtu = 23;
    AppInterface.TxFlushAll(); // pending responses belong to the dead session
    AppbleResetSessionBreadcrumbs();
}

static void BleSessionEnded(uint16_t Handle)
{
    BleSessionEnded(Handle, StaticBleLatch.Session == Handle);
}

// One supervision pass over the latch (called from AppBLETick, app task). `BleLatchTick` decides;
// this performs the stack call and the session teardown the decision implies.
static void BleLatchSupervise()
{
    const uint16_t SessionBefore = StaticBleLatch.Session;
    const BleLatchAction A = BleLatchTick(StaticBleLatch, BleHandleLive(StaticBleLatch.Drop),
                                          BleHandleLive(SessionBefore), TimeFromBoot());
    if (A.Step == BleLatchStep::Drop)
    {
        DeviceLog("APPBLE", "handle %u is still up after its drop: asking again (attempt %u, retries=%u)",
                  (unsigned)A.Handle, (unsigned)StaticBleLatch.Attempts, (unsigned)StaticBleLatch.Retries);
        if (BleServer)
            BleServer->disconnect(A.Handle);
        return;
    }
    if (A.Step == BleLatchStep::Heal)
    {
        // The tick has already cleared the latch, so the session-ness has to come from the state
        // BEFORE it ran - re-deriving it from the latch is what made the teardown a no-op.
        const bool WasSession = (A.Handle == SessionBefore);
        const bool Listed     = BleHandleLive(A.Handle);
        if (WasSession)
            DeviceLog("APPBLE", "%s handle %u: freeing the session latch (heals=%u forced=%u)",
                      Listed ? "the stack will not terminate a link that became a session,"
                             : "the link is gone but onDisconnect never came for",
                      (unsigned)A.Handle, (unsigned)StaticBleLatch.Heals, (unsigned)StaticBleLatch.Forced);
        else
            DeviceLog("APPBLE", "%s handle %u: the latch's record is cleared, the session is untouched "
                                "(retries=%u forced=%u heals=%u)",
                      Listed ? "the stack will not terminate a link that never held the session;"
                             : "the refused link is gone without onDisconnect;",
                      (unsigned)A.Handle, (unsigned)StaticBleLatch.Retries, (unsigned)StaticBleLatch.Forced,
                      (unsigned)StaticBleLatch.Heals);
        BleSessionEnded(A.Handle, WasSession);
    }
}


// ===== Connectable window =====
//
// A window is the PAIRING state. While one is open any phone may connect and pair (up to the three
// bonds the store holds). It is opened by a WRITE to the `BLE Settings` block's Window field (USB
// only, write the milliseconds you want; 0 closes it) and by the device itself, unattended, for
// BLE_AUTO_WINDOW_MS - but only after an EVENT the owner caused: the radio coming up with no bond
// stored (headless first use), or an explicit forget emptying the store (zeroing the `Bonds` array,
// a passkey change, the epoch revoke). It is deliberately NOT "the bond store reads empty" any more
// (Issues.md E69): the vendored stack deletes a bond when a peer that it already has a bond for asks
// to pair again, before any application callback runs, so a link could empty the store and make this
// device offer itself to the room for five minutes. Blocks/BleWindowPolicy.h holds that rule;
// AppBLETick consumes the event.
// The core's button is not a trigger: Button.h shares LED_NOTIFICATION_PIN with the notification LED
// and forces the button state false while the LED is driven, so a press is invisible exactly when it
// blinks.
//
// Advertising does NOT depend on a window: like any peripheral it is on the air whenever there is no
// live connection (BleAdvWanted; owner ruling, Issues.md E64). What a window decides is who may
// BOND, and with it who may hold a session: a window (or an empty store) is the only state in which
// a pairing may create a bond (BleBondingAllowed), so with a bond stored and no window the only
// peer that can come up encrypted AND bonded is the one that already had a key - the phone that owns
// the device. That is how the owner reconnects with no USB host present.
//
// The controller's accept list was E64's way of doing this in the link layer, and E66 killed it: the
// list is matched against the address the peer uses on the air, and a phone under LE privacy
// connects from a rotating private address (RPA) that this controller resolves for the connection
// but not for the filter - so the filter dropped the peer it existed to admit. The cost of dropping
// it is that a stranger's CONNECT_REQ is answered before the link is torn down; since E73 the
// teardown happens in onConnect, before any security procedure, so the stranger's OS never gets as
// far as a pairing prompt. See the advertising rule below, Issues.md E66 and E73.
// A radio that the block has disabled advertises nothing at all.

int AppBLEBondCount(); // defined with the bond read-out below

bool AppBLEWindowOpen()
{
    return BleRadioUp && BleEnabled && BleWindowEndMs != 0;
}

// Milliseconds left in the window, 0 when it is closed (the block's Window field).
uint32_t AppBLEWindowLeftMs()
{
    if (!AppBLEWindowOpen())
        return 0;
    const uint32_t Now = TimeFromBoot();
    const int32_t Left = (int32_t)(BleWindowEndMs - Now);
    return Left > 0 ? (uint32_t)Left : 0;
}

// Opens (or extends) the connectable window for `Ms`, or closes it when `Ms` is 0. Called from a
// write to the block's Window field, in the app task, so the deadline is a command and the read-back
// is the countdown (AppBLEWindowLeftMs). Returns false when the radio cannot offer itself at all -
// a radio the block has switched off has nothing to advertise; closing is always accepted.
bool AppBLESetWindow(uint32_t Ms)
{
    if (Ms == 0)
    {
        if (BleWindowEndMs != 0)
            DeviceLog("APPBLE", "connectable window closed by the block");
        BleWindowEndMs = 0;
        return true;
    }
    if (!BleRadioUp || !BleEnabled)
    {
        DeviceLog("APPBLE", "window refused: the radio is disabled");
        return false;
    }
    // The countdown compares signed (AppBLEWindowLeftMs), so a deadline further away than INT32_MAX
    // ms would read as already past. Refused rather than clamped: a write that asked for ~25 days of
    // pairing window is a mistake, not a request to be rounded.
    if (Ms > (uint32_t)INT32_MAX)
    {
        DeviceLog("APPBLE", "window refused: %u ms is longer than the countdown can hold", (unsigned)Ms);
        return false;
    }
    BleWindowEndMs = TimeFromBoot() + Ms;
    DeviceLog("APPBLE", "connectable window open for %u ms", (unsigned)Ms);
    return true;
}

// The automatic window: five minutes of discoverability the device owes to an explicit event - a
// radio that came up with no bond stored, a forget that emptied the store (Blocks/BleWindowPolicy.h,
// Issues.md E69), or a GPIO8 reset performed by a person at the device. Never opened because the
// store merely reads empty: the caller decides that with BleAutoWindowGranted, and the store is asked
// again here so a bond made in the meantime wins - except for `BleWindowCauseReset`, the one cause
// that grants over a stored bond (re-pairing without USB is what the input is for; a physical press
// cannot be produced by a link, which is the property E69 needed).
//
// One caller (AppBLETick), one deadline (BleWindowEndMs): the reset does not open the window
// itself, it names its own cause and lets the tick do it, so a reset and the automatic rule cannot
// each set a different deadline for the same event.
bool AppBLEAutoWindow(BleWindowCause Cause)
{
    if (!BleRadioUp || !BleEnabled)
        return false;
    if (Cause != BleWindowCauseReset && AppBLEBondCount() != 0)
        return false;
    const uint32_t NewEnd = TimeFromBoot() + BLE_AUTO_WINDOW_MS;
    if (BleWindowEndMs != 0 && (int32_t)(NewEnd - BleWindowEndMs) <= 0)
        return true; // a longer window is already open
    BleWindowEndMs = NewEnd;
    DeviceLog("APPBLE", "window open for %u ms (%s)", (unsigned)BLE_AUTO_WINDOW_MS,
              BleWindowCauseName(Cause));
    return true;
}

// ===== Bond store read-out / erasure (the block's Bonds array) =====
//
// Order and eviction (settled against this build's NimBLE):
//   - The store keeps entries in the order they were added (ble_store_config_peer_secs is an
//     append array, re-sorted by bond_count at boot), so entry 0 is the OLDEST bond and
//     getBondedAddress(i) walks oldest first - the order the block's Bonds array publishes.
//   - Capacity is CONFIG_BT_NIMBLE_MAX_BONDS = 3 (static_assert'd against the array's entry count).
//     When a fourth phone pairs, the config store answers BLE_HS_ESTORE_CAP, and the default
//     store-status handler that esp-nimble-cpp installs (ble_store_util_status_rr via
//     NimBLEDeviceCallbacks) calls ble_gap_unpair_oldest_peer() and retries: the OLDEST bond is
//     evicted, so the phone that paired first is the one that has to pair again. Nothing is
//     silently dropped - the store is full, one entry goes.
//   - Because that eviction and the new bond land inside ONE store-status callback, the size of
//     the store reads 3 before and 3 after while its content changed entirely. The block's array
//     therefore cannot be refreshed on the count alone (it used to be, and kept listing the evicted
//     phone): every completed bond and every forget bumps BleBondGeneration, and BleSettingsSync
//     re-formats on a change of it. The count and a slow content check in the glue are the safety
//     net for a store that changes without either event.

// "The bond store's content may differ from what the block last formatted". Bumped from the
// NimBLE host task (onAuthenticationComplete) and the app task (AppBLEForgetAllBonds,
// AppBLEForgetBond); read by the main loop, so volatile - a 32-bit aligned word, which the C3
// updates atomically.
static volatile uint32_t BleBondGeneration;

void AppBLEBondStoreChanged()
{
    BleBondGeneration = BleBondGeneration + 1;
}

uint32_t AppBLEBondGeneration()
{
    return BleBondGeneration;
}

int AppBLEBondCount()
{
    return BleRadioUp ? NimBLEDevice::getNumBonds() : 0;
}

// Writes bond `Index`'s identity address into `Out` in the order the block publishes it
// (AA:BB:CC:DD:EE:FF -> Out[0..5]). NimBLE keeps the six bytes least-significant first, which is
// the reverse of that, so the copy is what makes the block's entry readable as an address.
// False when the store holds no such entry.
bool AppBLEBondAddress(int Index, uint8_t *Out)
{
    if (!Out || !BleRadioUp || Index < 0 || Index >= NimBLEDevice::getNumBonds())
        return false;
    const uint8_t *Val = NimBLEDevice::getBondedAddress(Index).getVal();
    for (uint8_t i = 0; i < BLE_BOND_ADDR_LEN; i++)
        Out[i] = Val[BLE_BOND_ADDR_LEN - 1 - i];
    return true;
}

// ===== Link admission (Issues.md E66) =====
//
// A session needs a bond the peer can prove, and the owner of the device is the only peer that has
// one when no window is open. The controller cannot say which peer that is - the accept list only
// sees the address on the air (E66) - so the question is answered here, by comparing the identity
// address the stack ends up with against the bonds that existed when the link came up
// (Blocks/BleAdmissionPolicy.h has the rule itself, and test/native/ble_admission_test.cpp the
// table).
//
// NimBLEAddress compares the raw type byte, and the two sides may not use the same one: the bond
// store keeps 0/1 (public/random) while the controller's connection-complete event reports a peer
// whose RPA it resolved as 2/3 (public/random IDENTITY). NimBLE's `ble_hs_conn_addrs()` normalises
// 2/3 back to 0/1 on `peer_id_addr` before the host sees it, but the comparison is written for both
// type spaces anyway - it costs nothing and keeps the gate right if that ever stops being true.
// BleSameIdentity compares the six bytes and the public/random axis.
static void AppBLELinkBondSnapshot()
{
    const int Bonds = AppBLEBondCount();
    BleLinkBondCount = (uint8_t)(Bonds > CONFIG_BT_NIMBLE_MAX_BONDS ? CONFIG_BT_NIMBLE_MAX_BONDS : Bonds);
    for (uint8_t i = 0; i < BleLinkBondCount; i++)
        BleLinkBonds[i] = NimBLEDevice::getBondedAddress(i);
    // E64's pairing rule, decided once per link: a pairing on this link may only make a bond while
    // a window is open or while the store is still empty.
    BleLinkBondingClosed = !BleBondingAllowed(AppBLEWindowOpen(), Bonds);
}

// True when `Addr` is one of the bonds the store held when the link came up.
static bool BlePeerWasBonded(const NimBLEAddress &Addr)
{
    for (uint8_t i = 0; i < BleLinkBondCount; i++)
    {
        if (BleSameIdentity(BleLinkBonds[i].getVal(), BleLinkBonds[i].getType(),
                            Addr.getVal(), Addr.getType()))
            return true;
    }
    return false;
}

// ===== Bonds vs the passkey they were made under =====
//
// A bond is only worth anything under the passkey that created it: the phone comes back encrypted
// and bonded but NOT authenticated, and the connect gate refuses that - permanently, because a
// phone does not re-pair while it still believes it is bonded. So the code the store was built under
// is stamped (NVS, right beside the passkey; BlePasskeyStore.h), a completed bond stamps it, and any
// change of code forgets the bonds it invalidates. Without this the owner's own upgrade path -
// pair Just Works, then set a passkey - leaves the phone locked out, and rotating the code would
// revoke nothing.
static char BleBondEpoch[BLE_PASSKEY_LEN]; // the passkey text the stored bonds were made under
static bool BleBondEpochKnown = false;     // false = no stamp (fresh partition, or pre-rule upgrade)

// Drops every bond: a phone that was bonded is a stranger afterwards and has to pair again inside
// a window.
//
// The guard is the server, not BleRadioUp: at radio start the store has to be revocable (a passkey
// changed while the radio was off is applied there) before BleRadioUp is set at the end of that
// function. A created server means NimBLE is initialised, which is all the bond API needs.
//
// The advertisement is taken OFF THE AIR for the call (Issues.md E65). `ble_gap_unpair()` refuses
// with `BLE_HS_EBUSY` while advertising or discovery is active (ble_gap.c: "We cannot delete entry
// from resolving list if there is ongoing discovery or advertising in progress"), and since E64
// this device advertises whenever no session is up - which is the state a USB command arrives in.
// Without this, an idle device could not forget at all: the forget command answered FLAG_FAIL and kept
// the store, and the passkey-change revoke (AppBLEEnforceBondPolicy) revoked nothing. The delete
// runs on the GAP procedure on purpose - `ble_store_util_delete_peer` would sidestep the air, but
// it also skips the controller resolving-list removal that makes a forgotten peer unlinkable
// again, and a revoke has to do both. The advertisement is rebuilt straight afterwards by the
// tick's own path, so the air is empty for microseconds, not for a loop.
bool AppBLEForgetAllBonds()
{
    if (!BleServer)
        return false;

    NimBLEAdvertising *Adv = NimBLEDevice::getAdvertising();
    const bool AdvWasUp = Adv != nullptr && Adv->isAdvertising();
    bool AdvStopped = !AdvWasUp;
    if (AdvWasUp)
    {
        Adv->stop(); // a synchronous HCI command: the controller is idle once it returns
        AdvStopped = !Adv->isAdvertising();
        if (!AdvStopped)
            DeviceLog("APPBLE", "advertising would not stop: the bond delete will be refused (E65)");
    }

    const int Before = NimBLEDevice::getNumBonds();
    const bool Ok = NimBLEDevice::deleteAllBonds();

    // An empty store is under whatever code is current, so stamping it here is what keeps the
    // next bond - and a reboot - from looking like a stale store (see AppBLEEnforceBondPolicy).
    //
    // The stamp follows the STORE, not the attempt: a delete that left entries behind (a corrupt
    // or unwritable bond store) must not be recorded as done. Stamping then would tell the next
    // AppBLEEnforceBondPolicy that those bonds were made under the current code, so it would never
    // retry them and the phone the passkey change was meant to free would stay locked out. With
    // the epoch left as it was, the next radio start sees the mismatch and tries again.
    const int After = NimBLEDevice::getNumBonds();
    if (After == 0)
    {
        memcpy(BleBondEpoch, StaticVol.BleSet.Passkey, BLE_PASSKEY_LEN);
        BleBondEpochKnown = true;
        BleBondEpochSave(BleBondEpoch);

        // This IS the explicit event the automatic window is owed to (zeroing `Bonds`, a passkey change,
        // the epoch revoke - every one of them comes through here): every phone that was paired has
        // to pair again, so the device offers itself for BLE_AUTO_WINDOW_MS without being asked.
        // Gated on the store really coming up empty, like the stamp above: a delete that left an
        // entry behind has no peers to invite back.
        if (BleAutoWindowGranted(BleWindowCauseForgotten, After))
            BleAutoWindowCause = BleWindowCauseForgotten;
    }
    else
    {
        DeviceLog("APPBLE", "bond forget left %d bond(s): epoch not stamped, the next radio start retries",
                  After);
    }

    DeviceLog("APPBLE", "bonds forgotten: %d -> %d (ok=%d)", Before, After, Ok);
    AppBLEBondStoreChanged(); // the block's Bond texts follow in the next loop pass

    // The advertisement goes back up through AppBLETick's own rebuild path, in this same loop pass:
    // the register write that asked for the forget and the tick both run from the app task
    // (AppInterface::Pump dispatches the frame and then calls AppBLETick). Clearing the applied flag
    // IS the request - the tick rebuilds and starts the advertisement as soon as the air is free.
    if (AdvWasUp && AdvStopped)
    {
        BleAdvStateApplied = false;
        DeviceLog("APPBLE", "advertisement returns: the bond delete is done (E65)");
    }

    // The caller is told the STATE, not the per-entry result: an empty store afterwards is the
    // forget having happened, and it is also what the block's Bond fields will read back, so the
    // reply cannot disagree with the read-out. `Ok` stays in the log for the case where one unpair
    // reported an error over an entry the store had already lost.
    return After == 0;
}

// Forgets ONE bond: the peer at `Addr` (the published six bytes - the identity address the block's
// `Bonds` entry carries). Called from a `Bonds` write whose entry's address was zeroed.
//
// It is the same air dance as AppBLEForgetAllBonds and for the same reason (E65: `ble_gap_unpair()`
// refuses with BLE_HS_EBUSY while advertising, and this device advertises whenever no session is
// up - the state a USB command arrives in), and the rebuild is again requested, not started, so
// there is one start site (the tick).
//
// The epoch stamp and the automatic window are NOT touched here: emptying the store is
// AppBLEForgetAllBonds' job (the block calls it when every entry of the written array is zero), so
// the "an explicit forget emptied the store" event still has exactly one grant site.
bool AppBLEForgetBond(const uint8_t Addr[BLE_BOND_ADDR_LEN])
{
    if (!BleServer)
        return false;

    // The store is the truth about what exists; the entry being written only says what the host
    // wants. A bond the store does not hold is already gone, which is the state the write asked for.
    int Found = -1;
    uint8_t Have[BLE_BOND_ADDR_LEN];
    for (int i = 0; i < NimBLEDevice::getNumBonds(); i++)
        if (AppBLEBondAddress(i, Have) && memcmp(Have, Addr, BLE_BOND_ADDR_LEN) == 0)
        {
            Found = i;
            break;
        }
    if (Found < 0)
        return true;

    const NimBLEAddress Target = NimBLEDevice::getBondedAddress(Found);

    NimBLEAdvertising *Adv = NimBLEDevice::getAdvertising();
    const bool AdvWasUp = Adv != nullptr && Adv->isAdvertising();
    bool AdvStopped = !AdvWasUp;
    if (AdvWasUp)
    {
        Adv->stop(); // a synchronous HCI command: the controller is idle once it returns
        AdvStopped = !Adv->isAdvertising();
        if (!AdvStopped)
            DeviceLog("APPBLE", "advertising would not stop: the bond delete will be refused (E65)");
    }

    const int Before = NimBLEDevice::getNumBonds();
    const bool Ok = NimBLEDevice::deleteBond(Target);
    const int After = NimBLEDevice::getNumBonds();
    DeviceLog("APPBLE", "bond forgotten: %d -> %d (ok=%d)", Before, After, Ok);
    AppBLEBondStoreChanged();

    if (AdvWasUp && AdvStopped)
    {
        BleAdvStateApplied = false;
        DeviceLog("APPBLE", "advertisement returns: the bond delete is done (E65)");
    }

    // The caller is told the store's STATE, like the forget-all path: the block reads the array back
    // from the store, so a reply that disagreed with it would be the only place the truth differed.
    return After < Before;
}

// Records the policy the store is under. Cheap and idempotent: only a genuine change reaches NVS,
// which is why a reconnect does not wear the partition.
void AppBLEBondEpochStamp()
{
    if (BleBondEpochKnown && memcmp(BleBondEpoch, StaticVol.BleSet.Passkey, BLE_PASSKEY_LEN) == 0)
        return;
    memcpy(BleBondEpoch, StaticVol.BleSet.Passkey, BLE_PASSKEY_LEN);
    BleBondEpochKnown = true;
    // A refusal only costs a comparison after the next boot (the store is then forgotten once);
    // the RAM copy keeps this boot's decision correct either way.
    BleBondEpochSave(BleBondEpoch);
}

// Called whenever the policy can have changed - AppBLEApplySecurityPolicy (a passkey written while
// the radio is up) and AppBLERadioStart (one written while the radio was off, which is the same
// boot). Forgets the bonds if they were made under a different code.
//
// Returns the decision, not a status: true means the code in force is NOT the one the store was
// built under, i.e. this call IS a credential change - the bonds (if any) were invalidated and any
// live session was admitted under a code that no longer exists. False covers both "the text did not
// change" (a re-written passkey revokes nothing) and "an unstamped Just Works store was adopted",
// and neither may cost a live link. AppBLEApplySecurityPolicy drops the session on exactly this
// answer, so the forget and the drop can never disagree.
bool AppBLEEnforceBondPolicy()
{
    // The bond store's API needs the host; before createServer() there is nothing to compare
    // against, and the radio start calls this again once the store exists.
    if (!BleServer)
        return false;

    if (!BleBondEpochKnown)
        BleBondEpochKnown = BleBondEpochLoad(BleBondEpoch);

    const char *Now = StaticVol.BleSet.Passkey;

    // An unstamped store predates this rule, and the only build that could have written it had no
    // passkey at all - its bonds are Just Works bonds. Under a passkey they can never authenticate,
    // so they go; under Just Works they are exactly what the current policy asks for, so they are
    // kept and merely stamped (no re-pairing for every device that upgrades).
    if (!BleBondsInvalidated(BleBondEpochKnown, BleBondEpoch, Now))
    {
        AppBLEBondEpochStamp(); // nothing to revoke: just record which code the store is under now
        return false;
    }

    const int Bonds = NimBLEDevice::getNumBonds();
    if (Bonds > 0)
    {
        // The phone that paired under the old code cannot produce an authenticated link, and its
        // stored key would keep it connecting - it has to pair again inside a window.
        DeviceLog("APPBLE", "passkey changed since the bonds were made: forgetting %d bond(s)", Bonds);
        AppBLEForgetAllBonds(); // re-stamps the epoch only if the store really came up empty
        return true;
    }

    // An empty store is under whatever code is current: record it, or every radio start would
    // compare against a stamp that is gone.
    AppBLEBondEpochStamp();
    return true;
}

// Applies a changed passkey policy to the radio. Effective for the next pairing; the ATT
// permissions baked into the GATT table at init are not rebuilt (that needs a reboot), which is
// why the connect gate re-checks the policy on every link instead of trusting the table alone.
void AppBLEApplySecurityPolicy()
{
    if (!BleRadioUp)
        return;
    const BleSecurityPolicy Policy = BlePolicyNow();
    NimBLEDevice::setSecurityAuth(true, Policy.Mitm, true);
    NimBLEDevice::setSecurityIOCap(Policy.IoCap);
    NimBLEDevice::setSecurityPasskey(Policy.Passkey);
    DeviceLog("APPBLE", "policy applied: mitm=%u iocap=%u passkey set=%u",
              Policy.Mitm, Policy.IoCap, Policy.Mitm);

    // The bonds that were made under the previous code are worthless now - and would keep the
    // phone connecting with a bond it can never authenticate. Done after the policy is in force,
    // so the next connection inside a window pairs fresh and gets the prompt.
    //
    // The drop is decided by the SAME answer that decides the forget, and by nothing else:
    //   - a write that does not change the code revokes nothing AND drops nothing. The old test
    //     was "is MITM in force now", so re-writing the same passkey from the app disconnected a
    //     live, correctly authenticated session for no reason - a no-op write must be a no-op.
    //   - a real change drops whatever link is up, whichever way the code moved. A one-way MITM
    //     test missed passkey -> `000000`: MITM is off afterwards, so the peer that authenticated
    //     with the old code kept its session over a bond the same apply had just deleted. A link
    //     is admitted under the code in force when it came up, so after a change it is a stranger
    //     by definition - the phone has to pair again inside a window.
    const bool Revoked = AppBLEEnforceBondPolicy();
    if (Revoked && BleLatchHeld(StaticBleLatch))
    {
        DeviceLog("APPBLE", "dropping the live session: the passkey it authenticated with is gone");
        BleDropLink(StaticBleLatch.Session, "passkey changed under it");
    }
}

class BleServerCallbacks : public NimBLEServerCallbacks
{
    // A connection is NOT an app session. It becomes one only when the link is encrypted,
    // bonded - and, with a passkey set, authenticated - which is what onAuthenticationComplete
    // reports. Until then BleConnected stays false: no notification is sent and the GATT flags
    // refuse every read and write at the ATT layer.
    void onConnect(NimBLEServer *PServer, NimBLEConnInfo &ConnInfo) override
    {
        // Every link's lifetime is measured from here (onDisconnect logs the delta): a refusal that
        // lands in this callback has to read as milliseconds, and a session reads as its duration.
        BleLinkUpMs = TimeFromBoot();

        // One live connection at a time. The Tamu protocol has a single receive stream and one
        // session state, so a second peer would interleave with the first app's traffic rather
        // than get its own session. The controller allows three connections
        // (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3 because that is what the stack is built for), so
        // the refusal has to be here: the second phone is disconnected immediately and its GATT
        // access never reaches a handler. The phone sees a normal "disconnected by the device",
        // and the log names the reason.
        //
        // Every refusal below arms the pairing deadline BEFORE it returns and asks for the drop
        // through BleDropLink, which records the request: a refused link that the stack will not
        // tear down is then on a clock and under supervision instead of leaving the latch (or a
        // zombie ACL) behind with nothing watching it.
        // A latched link the stack no longer holds is not a session: it is the stuck latch this
        // whole mechanism exists for. Clearing it HERE means the connect that would have been
        // refused is admitted instead - "self-healing" from the phone's side has to mean the next
        // connect works, not that a retry loop eventually does (the tick's own pass would get there
        // within BLE_LATCH_CHECK_MS, but the peer in front of us would already have been dropped).
        if (BleLatchHeld(StaticBleLatch) && !BleHandleLive(StaticBleLatch.Session))
        {
            DeviceLog("APPBLE", "stale session latch on handle %u (the stack has no such link): clearing it",
                      (unsigned)StaticBleLatch.Session);
            StaticBleLatch.Heals = StaticBleLatch.Heals + 1;
            BleSessionEnded(StaticBleLatch.Session, /*WasSession=*/true);
        }

        if (!BleLatchTake(StaticBleLatch, ConnInfo.getConnHandle()))
        {
            DeviceLog("APPBLE", "refused a second peer: a link is already live (handle %u)",
                      (unsigned)StaticBleLatch.Session);
            BleArmSecureDeadline(ConnInfo.getConnHandle(), BLE_REFUSAL_GRACE_MS, BlePolicyNow().Mitm);
            BleDropLink(ConnInfo.getConnHandle(), "second peer");
            return;
        }

        BleMtu = 23;
        // The link's own chunk bound is not known until its MTU exchange; until then only the
        // absolute ceiling applies (Blocks/BleRxAssembler.h).
        StaticBleRxAssembler.MaxNeed = BLE_STREAM_CHUNK_ABS_MAX;
        BleSecured = false;
        BleConnected = false;
        StaticBleRxAssembler.Reset();
        BleParser.Reset();
        AppInterface.TxFlushAll();
        // What the bond store held before this link could change it - the session gate's question
        // (E66), and whether this link may create a bond at all.
        AppBLELinkBondSnapshot();

        // The block can switch the radio off at any moment; a connection that raced the switch is
        // dropped rather than served.
        if (!BleEnabled)
        {
            DeviceLog("APPBLE", "refused: the radio is disabled");
            BleArmSecureDeadline(ConnInfo.getConnHandle(), BLE_REFUSAL_GRACE_MS, BlePolicyNow().Mitm);
            BleDropLink(ConnInfo.getConnHandle(), "the radio is disabled");
            return;
        }

        // Who may HOLD a link is decided by a bond the peer can prove, not by the address it
        // arrives on (E66, Blocks/BleAdmissionPolicy.h) - and that question is answered HERE, at the
        // link, before the security procedure (Issues.md E73). With a bond stored and no window, a
        // peer whose identity is not one of those bonds can never become a session: the session gate
        // in onAuthenticationComplete would ask the same question and say no, and the security
        // procedure could only have failed, because the LTK lookup goes by identity address
        // (`ble_sm_retrieve_ltk`). Asking it now means the peer's security procedure never starts, so
        // its OS has no pairing dialog to raise: it reports a connection failure instead of a
        // pairing that appears to work and then fails.
        //
        // The address TYPE is deliberately not consulted. An earlier shape refused only peers on a
        // PUBLIC identity and let a private one through to the pairing exchange (E66's caution about
        // the controller's filter) - which is exactly the case a phone under LE privacy arrives in,
        // and where the pairing dialog the owner saw came from. The host does not need to resolve
        // anything here: when the controller resolves the peer's RPA, the connection-complete event
        // carries the IDENTITY address (types 2/3, normalised to 0/1 by `ble_hs_conn_addrs`) and
        // `BlePeerWasBonded` matches it against the store; when it does not resolve, the link is a
        // stranger's by definition and could not have authenticated anyway.
        if (BleConnectRefused(AppBLEWindowOpen(), BleLinkBondCount,
                              BlePeerWasBonded(ConnInfo.getIdAddress())))
        {
            // The line names the peer both ways (the identity the gate compared, the address it
            // came in on) and the reason, because "not bonded" and "window closed" are the two
            // inputs a rig session cannot read back anywhere else (the block has no link record).
            DeviceLog("APPBLE", "refused %s (identity %s): not bonded, window closed - no pairing started",
                      ConnInfo.getAddress().toString().c_str(),
                      ConnInfo.getIdAddress().toString().c_str());
            BleArmSecureDeadline(ConnInfo.getConnHandle(), BLE_REFUSAL_GRACE_MS, BlePolicyNow().Mitm);
            BleDropLink(ConnInfo.getConnHandle(), "not bonded, window closed");
            return;
        }

        // From here the link is judged, not yet admitted: the security procedure decides, and a link
        // that never completes it (the deadline in AppBLETick) is neither an admission nor one of
        // the refusals above.
        DeviceLog("APPBLE", "link up: peer=%s t=%u air=%s t=%u%s",
                  ConnInfo.getIdAddress().toString().c_str(),
                  (unsigned)ConnInfo.getIdAddress().getType(),
                  ConnInfo.getAddress().toString().c_str(),
                  (unsigned)ConnInfo.getAddress().getType(),
                  BleCameInResolved(ConnInfo.getAddress().getVal(), ConnInfo.getAddress().getType(),
                                    ConnInfo.getIdAddress().getVal(), ConnInfo.getIdAddress().getType())
                      ? " (resolved: the address on the air is the peer's RPA, not its identity)" : "");

        // A pairing on this link may not create a bond unless a window is open (or the store is
        // still empty, which is how the first bond is made). Without this a stranger could pair
        // outside a window, keep the bond and report itself bonded - first on this link, then on
        // every link after it. Set per link: the window moves under a running device, and the
        // stack reads the flag when a pairing starts.
        const BleSecurityPolicy Policy = BlePolicyNow();
        NimBLEDevice::setSecurityAuth(!BleLinkBondingClosed, Policy.Mitm, true);

        // Ask for security now instead of waiting for the first refused attribute access, so the
        // phone shows its pair dialog (and, with a passkey set, asks for the code) immediately.
        // A bonded phone answers with the key it stored, which is the whole point: that encryption
        // is the proof the session is gated on.
        const int Rc = ble_gap_security_initiate(ConnInfo.getConnHandle());
        if (Rc != 0)
            DeviceLog("APPBLE", "security initiate failed: rc=%d", Rc);

        // The pairing has to complete before this deadline. The budget comes from the policy being
        // enforced: with just works it waits for one tap, with a passkey it waits for a person to
        // read the prompt and type six digits (BleSecureBudgetMs; the old single 5 s figure dropped
        // every passkey pairing mid-procedure). AppBLETick drops the link when it passes and says
        // which case it was.
        BleArmSecureDeadline(ConnInfo.getConnHandle(), BleSecureBudgetMs(Policy), Policy.Mitm);
    }

    void onDisconnect(NimBLEServer *PServer, NimBLEConnInfo &ConnInfo, int Reason) override
    {
        // How long the link lived, from the connection-complete event to here. This is the timing
        // the connect-time refusal (Issues.md E73) is judged by: a refused link is a few
        // milliseconds old, a session as long as the app held it.
        DeviceLog("APPBLE", "link down after %u ms (secured=%u reason=0x%02x)",
                  (unsigned)(TimeFromBoot() - BleLinkUpMs), (unsigned)BleSecured, (unsigned)Reason);
        // Everything else a session owns (the latch, the deadline, the assembler, the TX ring) is
        // released in one place, shared with the latch's self-heal - see BleSessionEnded.
        (void)PServer;
        BleSessionEnded(ConnInfo.getConnHandle());
    }

    // Encryption change / pairing completed. This is where a link is judged: the app session is
    // established only for a link that meets the whole policy, and anything else is dropped.
    void onAuthenticationComplete(NimBLEConnInfo &ConnInfo) override
    {
        const BleSecurityPolicy Policy = BlePolicyNow();

        if (!ConnInfo.isEncrypted() || !ConnInfo.isBonded())
        {
            // With bonding closed for this link (a bond stored, no window) this is the stranger
            // case rather than a failed pairing: the link encrypted without a bond we could keep.
            DeviceLog("APPBLE", "link refused (enc=%u bonded=%u%s): dropping",
                      ConnInfo.isEncrypted(), ConnInfo.isBonded(),
                      BleLinkBondingClosed ? ", no pairing window" : "");
            BleSecured = false;
            BleConnected = false;
            BleArmSecureDeadline(ConnInfo.getConnHandle(), BLE_REFUSAL_GRACE_MS, Policy.Mitm);
            BleDropLink(ConnInfo.getConnHandle(), "not encrypted+bonded");
            return;
        }

        // With a passkey set, encryption without authentication means the pairing skipped the
        // code, so the link is refused even though encryption succeeded.
        if (Policy.Mitm && !ConnInfo.isAuthenticated())
        {
            DeviceLog("APPBLE", "unauthenticated link refused: a passkey is set");
            BleSecured = false;
            BleConnected = false;
            BleArmSecureDeadline(ConnInfo.getConnHandle(), BLE_REFUSAL_GRACE_MS, Policy.Mitm);
            BleDropLink(ConnInfo.getConnHandle(), "unauthenticated");
            return;
        }

        // The session gate (Issues.md E66). A link becomes a session only when it proves a bond we
        // already had: a window is open, the store is still empty (this pairing is the first bond),
        // or the peer is one of the bonds that existed when the link came up. Encryption alone is
        // not enough - a stranger who paired on a closed link is encrypted and bonded too, which is
        // exactly what the controller's accept list used to prevent and cannot (E66). Such a bond is
        // forgotten here, so the store it tried to enter cannot admit it on the next connect.
        if (!BleSessionAllowed(AppBLEWindowOpen(), BleLinkBondCount,
                               BlePeerWasBonded(ConnInfo.getIdAddress())))
        {
            DeviceLog("APPBLE", "refused a link that paired outside the pairing window: forgetting its bond");
            if (!NimBLEDevice::deleteBond(ConnInfo.getIdAddress()))
                DeviceLog("APPBLE", "could not forget the bond of %s: it still admits this peer",
                          ConnInfo.getIdAddress().toString().c_str());
            AppBLEBondStoreChanged();
            BleSecured = false;
            BleConnected = false;
            BleArmSecureDeadline(ConnInfo.getConnHandle(), BLE_REFUSAL_GRACE_MS, Policy.Mitm);
            BleDropLink(ConnInfo.getConnHandle(), "paired outside the window");
            return;
        }

        BleSecured = true;
        BleConnected = true;
        BleMtu = ConnInfo.getMTU();
        StaticBleRxAssembler.MaxNeed = BleMaxChunkForMtu(BleMtu);
        BleClearSecureDeadline();
        // The naming rule's trigger (Blocks/AutoName.h "WHEN"): this link came up with an EMPTY bond
        // store and has just made the device's first bond. Raised here and performed by the app
        // task on its next pass (Devices/Tamu_v2.0A/BleAutoName.h), because the write is a
        // System/Name write plus its `.SV` mirror, and a flash write does not belong in a
        // connection callback. A reconnect never raises it: its link began with a bond stored.
        if (BleLinkBondCount == 0)
            BleAutoNameOwed = true;
        // Prefer 2M PHY when the central supports it: halves air time per packet
        // (helps multi-notification transfers); stays at 1M otherwise.
        if (BleServer)
        {
            BleServer->updatePhy(ConnInfo.getConnHandle(),
                                 BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
            // BlueZ defaults to a long connection interval (~30-60 ms); request a
            // fast one so transactions do not wait multiple connection events.
            // Units: interval 1.25 ms (6..12 -> 7.5..15 ms), timeout 10 ms (400 -> 4 s).
            BleServer->updateConnParams(ConnInfo.getConnHandle(), 6, 12, 0, 400);
        }

        DeviceLog("APPBLE", "link secured: enc=%u bonded=%u auth=%u bonds=%d bits=%u peer=%s t=%u air=%s t=%u",
                  ConnInfo.isEncrypted(), ConnInfo.isBonded(), ConnInfo.isAuthenticated(),
                  NimBLEDevice::getNumBonds(), ConnInfo.getSecKeySize(),
                  ConnInfo.getIdAddress().toString().c_str(),
                  (unsigned)ConnInfo.getIdAddress().getType(),
                  ConnInfo.getAddress().toString().c_str(),
                  (unsigned)ConnInfo.getAddress().getType());

        // A bond exists that did not before - including the case where this pairing overflowed
        // the store and the stack evicted the oldest entry inside the same callback, which leaves
        // getNumBonds() unchanged. Tell the block to re-read the entries rather than the size.
        AppBLEBondStoreChanged();

        // The bond epoch is deliberately NOT stamped here. This link is bonded, but not
        // necessarily under the code in force: a bond made under an old passkey authenticates from
        // its stored LTK (it was paired with a passkey, so the key is marked authenticated), which
        // is exactly what the rig showed - the old bond kept full access after the passkey changed
        // (Issues.md E65, and E68 for this line). Stamping on a reconnect rewrote the epoch of a
        // store the revoke had failed to clear, so the next radio start found the two equal and
        // never retried, and the rotated code left that bond in place for good.
        //
        // The epoch therefore only moves where the STORE is known to be wholly under the current
        // code: an empty store (AppBLEForgetAllBonds) or one with nothing to revoke
        // (AppBLEEnforceBondPolicy, which also runs after every policy apply). A pairing that adds
        // a bond under an epoch that already matches changes nothing, so a reconnect now writes no
        // NVS entry at all - which is what the old comment claimed.
    }

    // The code the user configured, for the DISPLAY_ONLY policy. NimBLE-cpp only asks here when
    // the static passkey is left at its 123456 default, so returning the configured value keeps
    // a passkey of exactly 123456 working.
    uint32_t onPassKeyDisplay() override
    {
        return BlePolicyNow().Passkey;
    }

    void onMTUChange(uint16_t MTU, NimBLEConnInfo &ConnInfo) override
    {
        BleMtu = MTU;
        // The RX chunk assembler's bound follows the link it is reading: a chunk can never declare
        // more than this link can carry (Blocks/BleRxAssembler.h).
        StaticBleRxAssembler.MaxNeed = BleMaxChunkForMtu(MTU);
    }

    void onPhyUpdate(NimBLEConnInfo &ConnInfo, uint8_t TxPhy, uint8_t RxPhy) override
    {
        DeviceLog("APPBLE", "phy updated: tx=%u rx=%u", TxPhy, RxPhy);
    }

    void onConnParamsUpdate(NimBLEConnInfo &ConnInfo) override
    {
        // Diagnostic breadcrumb: confirms the fast-interval request was accepted
        // by the central (7..15 ms keeps transaction round trips short).
        DeviceLog("APPBLE", "conn params: itvl=%u.%02u ms mtu=%u",
                  ConnInfo.getConnInterval() * 125 / 100,
                  ConnInfo.getConnInterval() * 125 % 100,
                  ConnInfo.getMTU());
    }
} StaticBleServerCallbacks;

class BleTxCallbacks : public NimBLECharacteristicCallbacks
{
    // App -> device bytes arrive from the NimBLE host task: buffer only, no processing.
    // Writes carry length-prefixed chunks (uint16 LE + stream, Docs/Services/App
    // Interface.md) but BlueZ may deliver them fragmented or coalesced arbitrarily,
    // so every byte goes through the reassembler instead of assuming write==chunk.
    void onWrite(NimBLECharacteristic *PCharacteristic, NimBLEConnInfo &ConnInfo) override
    {
        std::string Value = PCharacteristic->getValue();
        size_t Len = Value.length();
        if (!LoggedFirstWrite)
        {
            LoggedFirstWrite = true;
            DeviceLog("APPBLE", "first write: %u Bytes", (unsigned)Len);
        }

        for (size_t i = 0; i < Len; i++)
        {
            uint8_t Byte = 0;
            const BleFeedResult F = StaticBleRxAssembler.Feed((uint8_t)Value[i], TimeFromBoot(), &Byte);
            if (F.Resynced)
            {
                // The chunk in flight was abandoned (its declared length cannot be carried, or it
                // stopped arriving). The byte that showed it starts a fresh prefix, but the PACKET
                // parser still holds the bytes of a frame that will never finish - so it is reset
                // with the assembler. This is the line that makes the recovery visible: before,
                // the session silently failed every CRC from here on.
                BleParser.Reset();
                DeviceLog("APPBLE", "RX chunk resync #%u (overruns=%u truncations=%u): dropped the partial chunk",
                          (unsigned)StaticBleRxAssembler.Resyncs,
                          (unsigned)StaticBleRxAssembler.Overruns,
                          (unsigned)StaticBleRxAssembler.Truncations);
            }
            if (!F.Stream)
                continue;
            // Assembled stream bytes go straight into the wire-stream parser;
            // complete frames are queued for the pump (no intermediate buffer).
            if (BleParser.Feed(Byte))
            {
                // A full inbound queue drops the frame, exactly as the USB reader's does: count it
                // (AppBusGateDrain's totals line), or the app's timeout looks like a node that
                // never answered when the request never became a transaction at all.
                if (!AppInterface.Enqueue(BleParser.Frame, AppLinkSource::BLE))
                    AppLinkEnqueueDropsBle = AppLinkEnqueueDropsBle + 1;
                BleParser.Reset();
            }
            else if (BleParser.Full)
            {
                DeviceLog("APPBLE", "Rejected malformed app frame (CRC)");
                AppLinkFrameRejectsBle = AppLinkFrameRejectsBle + 1; // resync, and say so
                BleParser.Reset(); // CRC failure: resync
            }
        }

    }
} StaticBleTxCallbacks;


// Trims the fixed 16-char space-padded System Name into a NUL-terminated C string (`Out` must
// hold SYSTEM_NAME_LEN + 1 bytes). The field carries no terminator, so it is copied trimmed for
// the places that need a C string (the NimBLE device name and the advertising name).
static void SystemNameCString(char *Out)
{
    uint8_t Len = SYSTEM_NAME_LEN;
    while (Len > 0 && StaticPer.System.Name[Len - 1] == ' ') Len--;
    memcpy(Out, StaticPer.System.Name, Len);
    Out[Len] = '\0';
}

// ===== Advertising rule =====
//
// One condition: advertising is on the air whenever the radio is up and enabled and there is no live
// connection - it does not depend on the bond store or on a window (owner ruling, Issues.md E64).
// A peripheral that is not connected is discoverable; anything else leaves a phone staring at a
// device that cannot be found. The "no connection" half is the deferred restart below: advertising
// is only started while `!BleConnected`, and a dropped link restarts it on the next tick.
static bool BleAdvWanted()
{
    return BleRadioUp && BleEnabled;
}

// The controller's accept list is NOT used, and E66 is why. It was E64's link-layer gate: with a
// bond stored and the window closed the advertising filter policy was whitelist-only, so a
// stranger's CONNECT_REQ was ignored by the controller instead of being answered and then torn
// down. That cannot work here. The list is matched against the address on the air, and a phone
// under LE privacy connects from a rotating private address (RPA); this controller holds the peer's
// IRK and resolves that RPA - which is how the stored bond authenticates on reconnect - but it does
// not apply the resolution to the accept-list filter, so the filter dropped the peer it existed to
// admit (`adv_filter_policy=2` with `entry-0={addr_type=0 addr=4c:23:38:74:e6:f6}` on the air,
// Issues.md E66). Nothing sets an advertising filter policy any more, and the bond store is no
// longer mirrored into the controller's white list: the admission question is answered by the
// connect gate (BleConnectRefused in onConnect) with the session gate behind it (BleSessionAllowed
// in onAuthenticationComplete).
//
// The trade-off, stated rather than hidden: a stranger's CONNECT_REQ is answered, and the link is
// torn down in onConnect within a few milliseconds - before its security procedure, so no pairing
// dialog is raised (E73; the refused link's own lifetime is logged by onDisconnect). The
// advertisement and its scan response were already unfiltered, so nothing new is visible; what is
// lost is that the controller no longer drops the request in the link layer.

// Rebuilds and starts the advertisement from scratch. Used for the initial
// start AND every post-session restart: restarting the previous instance can
// leave an active-but-invisible advertisement (observed on hardware).
static void StartAppAdvertising()
{
    NimBLEAdvertising *Adv = NimBLEDevice::getAdvertising();
    if (!Adv)
        return;

    Adv->stop();
    Adv->clearData();
    Adv->removeServices();
    bool UuidOk = Adv->addServiceUUID(BLE_SERVICE_UUID);
    Adv->enableScanResponse(true); // must precede setName (see ordering note)
    char AdvName[SYSTEM_NAME_LEN + 1];
    BleEffectiveName(AdvName); // the block's Display name, or System.Name
    bool NameOk = Adv->setName(AdvName);
    bool Started = Adv->start();
    // AFTER the start: the server's first start re-initialises the GAP service name from the
    // config default ("nimble") - see BleSetGapName. Same string as the advertisement above.
    const bool GapNameOk = BleSetGapName(AdvName);
    DeviceLog("APPBLE", "adv restart: uuid=%d name=%d started=%d gapname=%d filter=none",
              UuidOk, NameOk, Started, GapNameOk);
}

// Why the last radio start failed, in a form the log can name. A start that fails has to say WHICH
// call failed - "radio start failed" alone left the owner (and the rig) with no next step.
static const char *BleStartFail = "not attempted";

// Starts the radio: the NimBLE host, the pairing policy and the GATT table (built from that
// policy). Called at boot when the block's Enabled is true, and later if it is switched on.
// Idempotent per boot: BleRadioUp guards it.
static bool AppBLERadioStart()
{
    // The pairing policy is read from the `BLE Settings` block BEFORE the server exists, so the
    // characteristics are built with the permissions that policy implies.
    const BleSecurityPolicy Policy = BlePolicyNow();
    char Name[SYSTEM_NAME_LEN + 1];
    BleEffectiveName(Name);

    if (!NimBLEDevice::init(Name))
    {
        BleStartFail = "NimBLEDevice::init() returned false";
        DeviceLog("APPBLE", "radio start failed: %s", BleStartFail);
        return false;
    }
    NimBLEDevice::setMTU(512);

    // A: passkey "000000" -> Just Works: bonding on, MITM off, LE Secure Connections on, and the
    // device offers neither input nor output. B: a passkey is set -> MITM required and the
    // device only displays the code, which the phone asks the user for. The code is never typed
    // on the device (it has no display) - it arrives over USB in the block's Passkey field.
    NimBLEDevice::setSecurityAuth(true, Policy.Mitm, true);
    NimBLEDevice::setSecurityIOCap(Policy.IoCap);
    NimBLEDevice::setSecurityPasskey(Policy.Passkey);
    // Keep the encryption and identity keys on both sides: the identity key (IRK) is what lets a
    // phone's rotating private address resolve back to the bonded identity, and the bond is
    // useless for a reconnect without the LTK.
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

    BleServer = NimBLEDevice::createServer();
    if (!BleServer)
    {
        BleStartFail = "NimBLEDevice::createServer() returned null";
        DeviceLog("APPBLE", "radio start failed: %s", BleStartFail);
        return false;
    }
    BleServer->setCallbacks(&StaticBleServerCallbacks);

    NimBLEService *Svc = BleServer->createService(BLE_SERVICE_UUID);
    if (!Svc)
    {
        // A half-built radio is worse than none: it would advertise a service nobody can use. Say
        // which call failed and let the backoff decide whether to try again.
        BleStartFail = "createService() returned null";
        DeviceLog("APPBLE", "radio start failed: %s", BleStartFail);
        return false;
    }

    // Every characteristic permission requires encryption (the flags below carry the *_ENC bits),
    // plus *_AUTHEN when a passkey is set: the ATT server refuses an unencrypted or
    // unauthenticated peer at the attribute, not in application code.
    NimBLECharacteristic *Rx = Svc->createCharacteristic(BLE_RX_CHAR_UUID, Policy.RxFlags);
    Rx->setCallbacks(&StaticBleTxCallbacks);

    BleTx = Svc->createCharacteristic(BLE_TX_CHAR_UUID, Policy.TxFlags);
    if (!Rx || !BleTx)
    {
        BleStartFail = "createCharacteristic() returned null";
        DeviceLog("APPBLE", "radio start failed: %s", BleStartFail);
        return false;
    }

    // (NimBLEService::start() is a no-op / deprecated: services start with the server.)

    // Revoke the bonds the current passkey invalidates before anything can use them: a code
    // changed while the radio was down (Enabled = false is a USB-only write, so the store could not
    // be touched then) is applied here.
    AppBLEEnforceBondPolicy();

    const int Bonds = NimBLEDevice::getNumBonds();

    memcpy(BleAppliedName, Name, SYSTEM_NAME_LEN + 1);
    BleStartFail = "none";
    BleRadioUp = true;
    BleEnabled = true;
    // Do NOT start advertising here: AppBLETick owns that, and its first pass starts one straight
    // away - advertising does not wait for a bond or for a window.
    BleAdvRestart = false;
    BleWindowEndMs = 0;
    BleAdvStateApplied = false;

    // Headless first use: a radio that came up with no bond stored offers itself for the automatic
    // period, so a device nobody has paired with yet is usable from the phone alone. This is an
    // EVENT (the radio starting), not a reading of the store - a store emptied by a link is not one
    // (Issues.md E69, Blocks/BleWindowPolicy.h). AppBLEEnforceBondPolicy above may already have
    // emptied the store, in which case its forget left the event behind and this is the same one.
    if (BleAutoWindowGranted(BleWindowCauseRadioStart, Bonds))
        BleAutoWindowCause = BleWindowCauseRadioStart;

    DeviceLog("APPBLE", "radio up: name='%s' mode=%s bonds=%d",
              BleAppliedName, Policy.Mitm ? "passkey" : "just works", Bonds);
    return true;
}

// ===== A radio that will not start =====
//
// `AppBLESettingsApply` runs every main-loop pass (~500 Hz), and it used to call
// `AppBLERadioStart()` unconditionally whenever the block wanted the radio and it was not up. A
// start that fails (NimBLE's init, the server, the service, the characteristics) therefore ran
// again on the very next pass: ~500 init/createServer cycles a second on a half-initialised stack,
// which does not recover the radio - it just burns the task and makes the failure invisible.
//
// The attempt is now counted and spaced: BLE_START_RETRY_MS doubling per failure, and after
// BLE_START_MAX_ATTEMPTS the device gives up and says so, naming the call that failed. The give-up
// state is not silent and not permanent: a write to the block's Enabled field (a fresh enable) and
// the GPIO8 reset both clear it, because both are a person asking again.
#define BLE_START_MAX_ATTEMPTS 3
#define BLE_START_FIRST_RETRY_MS 2000

static uint8_t  BleStartAttempts = 0;  // starts since the last success (0 = nothing failed)
static uint32_t BleStartRetryMs  = 0;  // 0 = an attempt may run now
static bool     BleStartGaveUp  = false;

// Clears the backoff: a deliberate request to start the radio again.
static void BleRadioStartResetBackoff()
{
    BleStartAttempts = 0;
    BleStartRetryMs  = 0;
    BleStartGaveUp   = false;
}

// One start attempt, on the backoff. Called wherever the radio is meant to be up and is not.
static void BleRadioStartAttempt()
{
    const uint32_t Now = TimeFromBoot();
    if (BleStartGaveUp)
        return; // it has been said once already; only a fresh enable or a reset retries
    if (BleStartRetryMs != 0 && (int32_t)(Now - BleStartRetryMs) < 0)
        return; // still backing off

    if (AppBLERadioStart())
    {
        if (BleStartAttempts > 0)
            DeviceLog("APPBLE", "radio came up on attempt %u", (unsigned)(BleStartAttempts + 1));
        BleRadioStartResetBackoff();
        return;
    }

    BleStartAttempts = (uint8_t)(BleStartAttempts + 1);
    BleStartRetryMs  = 0;
    if (BleStartAttempts >= BLE_START_MAX_ATTEMPTS)
    {
        BleStartGaveUp = true;
        // The one line that matters: which call failed, how many times were tried, and what will
        // make the device try again. The radio is DOWN here - BleRadioUp stays false, so nothing
        // advertises - and advertising must never claim otherwise.
        DeviceLog("APPBLE", "radio start GIVEN UP after %u attempts (%s): the block's Enabled write "
                            "or the GPIO8 reset starts it again",
                  (unsigned)BleStartAttempts, BleStartFail);
        return;
    }

    const uint32_t Backoff = (uint32_t)BLE_START_FIRST_RETRY_MS << (BleStartAttempts - 1);
    BleStartRetryMs = Now + Backoff;
    DeviceLog("APPBLE", "radio start failed (%s), attempt %u/%u: retrying in %u ms",
              BleStartFail, (unsigned)BleStartAttempts, (unsigned)BLE_START_MAX_ATTEMPTS,
              (unsigned)Backoff);
}

// Brings the radio level with the block's Enabled and Display name fields. Called from the main
// loop (BleSettingsSync) so a write takes effect within one iteration; the pairing policy needs
// no call here - BlePolicyNow() reads the block at every use, and a written passkey applies
// through its trigger.
void AppBLESettingsApply()
{
    const bool WantEnabled = StaticPer.BleSet.Enabled != 0;
    char Name[SYSTEM_NAME_LEN + 1];
    BleEffectiveName(Name);

    if (WantEnabled && !BleRadioUp)
    {
        // First enable, or the boot of a device that had it on - or a retry of a start that failed.
        // The retry is on a backoff (BleRadioStartAttempt): a radio that will not come up must not
        // be re-initialised on every loop pass, which is what this did - NimBLEDevice::init() +
        // createServer() up to ~500 times a second on a half-initialised stack.
        BleRadioStartAttempt();
        return;
    }

    if (!WantEnabled && BleRadioUp)
    {
        // Off means off: no advertisement, no window, no session. The controller stays
        // initialised - tearing NimBLE down after the GATT table is registered is not a safe
        // runtime operation - but it transmits nothing until the block enables it again, and a
        // device that boots with Enabled = false never initialises the radio at all.
        if (BleEnabled)
        {
            BleEnabled = false;
            BleWindowEndMs = 0;
            BleClearSecureDeadline();
            BleAdvRestart = false;
            // The advertisement was stopped here, so the tick must not look for a transition.
            BleAdvStateApplied = false;
            if (BleLatchHeld(StaticBleLatch))
                BleDropLink(StaticBleLatch.Session, "the radio was disabled");
            NimBLEAdvertising *Adv = NimBLEDevice::getAdvertising();
            if (Adv)
                Adv->stop();
            DeviceLog("APPBLE", "radio disabled by the BLE Settings block");
        }
        return;
    }

    if (!BleRadioUp)
        return;

    BleEnabled = WantEnabled;

    // The advertised (and device) name follows the block.
    if (strcmp(Name, BleAppliedName) != 0)
    {
        // The characteristic first: a live session blocks the advertisement (the restart below is
        // deferred to the next tick after the link drops) but NOT a peer reading 0x2A00, so the two
        // values must not drift while the restart waits.
        BleSetGapName(Name);
        memcpy(BleAppliedName, Name, SYSTEM_NAME_LEN + 1);
        DeviceLog("APPBLE", "advertised name -> '%s'", BleAppliedName);
        // Re-advertise NOW, in this same pass: the field and the air are the same thing, and on a
        // device that advertises whenever it is idle, "at the next advertising start" can mean
        // never. A live session cannot advertise at all - the tick's deferred restart carries the
        // new name onto the air the moment the link drops.
        if (BleAdvWanted() && !BleConnected)
        {
            StartAppAdvertising(); // rebuild, never mutate in place (see its own note)
            BleAdvStateApplied = true;
            BleAdvRestart = false;
        }
        else
        {
            BleAdvRestart = true;
        }
    }
}

// ===== The USB-less reset (GPIO8) =====
//
// The owner's ruling is that the device must be recoverable without a cable: a contact held on GPIO8
// for 2..10 s restarts the BLE stack when it is released, and one held for 10 s clears the bonds
// first. Blocks/BleResetHold.h owns the timing and the LED; Devices/Tamu_v2.0A/BleResetPin.h feeds it
// the pin; this is the ACTION.
//
// Why a stack restart and not just an advertising rebuild: the point is to recover a wedged host or
// a controller state that no longer advertises, and to apply a fresh pairing state without a whole
// reset of the device (the app must reconnect afterwards). `NimBLEDevice::deinit(true)` is the
// stack's own shutdown - it stops the host task, de-initialises the controller and deletes the
// server/advertising objects (managed_components/h2zero__esp-nimble-cpp/src/NimBLEDevice.cpp) - and
// ESP-IDF's nimble_port_init() is written to start a new cycle after it. Everything this file holds
// a pointer to is owned by that deleted server, so the pointers are cleared here and the radio is
// rebuilt through AppBLERadioStart(), the ONE start site, rather than a second copy of it.
//
// Bonds: the sub-10 s path keeps them (the phone reconnects with its stored key). The 10 s path runs
// the same forget every other path uses (AppBLEForgetAllBonds: advertisement down for the unpair,
// E65, `deleteAllBonds()` -> `ble_gap_unpair()` per peer, the epoch stamp re-written only if the
// store really came up empty, and the "forgotten" window event queued). What survives in NVS: the
// passkey (namespace `ble`, key `passkey`) and the epoch stamp beside it - both untouched by a bond
// delete; the peer LABELS (BleBondLabelStore) are pruned by the next BleBondArrayFormat, which the
// generation bump in the forget schedules.
//
// The window: both paths open BLE_AUTO_WINDOW_MS (300 s), and the reset does NOT set the deadline
// itself. It names its own cause and the tick opens it (AppBLEAutoWindow), so there is one owner of
// `BleWindowEndMs` per event and the automatic rule cannot later open a second, different one for
// the same contact: the radio start inside this function queues its own cause and this assignment
// replaces it.
//
// The block stays the source of truth for whether the radio may be used at all: a device whose
// Enabled field is false comes back with the radio down (no advertisement), exactly as it was.
bool AppBLEStackReset(bool ForgetBonds)
{
    if (!BleServer)
    {
        // The radio was never started (Enabled false at boot): there is nothing on the air to
        // restart, and NimBLE was never initialised to take down.
        DeviceLog("APPBLE", "reset refused: the BLE radio was never started");
        return false;
    }

    DeviceLog("APPBLE", "reset: %s - restarting the BLE stack",
              ForgetBonds ? "clearing the bonds first" : "bonds kept");

    if (ForgetBonds && !AppBLEForgetAllBonds())
    {
        // The restart still happens: a store that would not empty is a reason to log, not a reason
        // to leave the owner without the reset they asked for. The next radio start retries the
        // revocation through AppBLEEnforceBondPolicy (the epoch was deliberately not stamped).
        DeviceLog("APPBLE", "reset: the bond store did not come up empty - restarting anyway");
    }

    // Shut the stack down. Everything below assumes it is gone: after this call the server, the
    // characteristics and the advertising object no longer exist.
    if (!NimBLEDevice::deinit(true))
        DeviceLog("APPBLE", "reset: deinit reported a failure - re-initialising anyway");

    BleServer = nullptr;
    BleTx = nullptr;
    BleRadioUp = false;
    BleEnabled = false;
    // Every handle the old stack knew is meaningless now: the latch and the deadline go with it.
    BleLatchClearAll(StaticBleLatch);
    BleSecured = false;
    BleConnected = false;
    BleOldConnected = false;
    BleAdvRestart = false;
    BleAdvStateApplied = false;
    BleWindowEndMs = 0;
    BleClearSecureDeadline();
    BleMtu = 23;
    BleLinkBondCount = 0;
    BleLinkBondingClosed = false;
    StaticBleRxAssembler.Reset();
    StaticBleRxAssembler.MaxNeed = BLE_STREAM_CHUNK_ABS_MAX;
    BleParser.Reset();
    AppInterface.TxFlushAll(); // pending responses belonged to the sessions that just died

    // A reset is a person asking for a fresh start, so it also clears a radio-start give-up: if the
    // stack can be brought back, this is the attempt that does it.
    BleRadioStartResetBackoff();
    if (!AppBLERadioStart())
    {
        DeviceLog("APPBLE", "reset: the radio did not come back up (%s)", BleStartFail);
        return false;
    }

    // The block's Enabled field is still the master switch: a radio the owner had switched off
    // comes back off (AppBLERadioStart brought it up with its settings, AppBLESettingsApply takes
    // it back down and stops the advertisement).
    if (StaticPer.BleSet.Enabled == 0)
        AppBLESettingsApply();

    // One owner of the window per event - this press. The radio start above may have queued its
    // own cause (it came up with an empty store on the forget path); the reset's cause replaces it.
    BleAutoWindowCause = BleWindowCauseReset;
    // The tick rebuilds and starts the advertisement (AppBLETick -> StartAppAdvertising), which is
    // also what puts the device back in front of the app. Requesting it by clearing the "applied"
    // flag is the same shape every other change to the advertisement uses.
    BleAdvStateApplied = false;
    return true;
}

void AppBLEInit(const char *DeviceName)
{
    // The fallback advertised name: the persisted System Name, used while the block's Display
    // name field is empty. From here on the block decides the rest.
    strncpy(BleFallbackName, DeviceName, SYSTEM_NAME_LEN);
    BleFallbackName[SYSTEM_NAME_LEN] = '\0';
    AppBLESettingsApply();
}



bool AppBLEActive()
{
    return BleConnected;
}

// Pumped from ApplicationTask: flushes pending TX data as notifications. Also restarts
// advertising after a disconnect (deferred out of the BLE callback context, retried until
// it succeeds). RX (assembler + parser) is fed from the NimBLE host task's GATT callbacks.
void AppBLETick()
{
    // Comm LED is a per-burst pulse: RX/TX below turn it on, and this tick turns
    // it back off at the start so an idle session does not leave it lit.

    // A device whose block has the radio disabled has no NimBLE host at all (Enabled was false
    // at boot) - nothing to advertise, nothing to time out.
    if (!BleRadioUp)
        return;

    // ---- connectable window ----
    // It closes by itself; the advertising rule and the connect filter both hang off it.
    if (BleWindowEndMs != 0 && (int32_t)(TimeFromBoot() - BleWindowEndMs) >= 0)
    {
        BleWindowEndMs = 0;
        DeviceLog("APPBLE", "connectable window closed");
    }

    // Automatic window, owed to an explicit event only (Issues.md E69, Blocks/BleWindowPolicy.h):
    // the radio starting with no bond stored, or an explicit forget that emptied the store. The
    // store reading empty on its own grants nothing - the vendored stack deletes a bond when a peer
    // that already has one asks to pair again, before any application callback runs, so a window
    // hung off the count let a link make this device offer itself. The event is consumed here, once,
    // so a window is opened per event and never per loop pass; AppBLEAutoWindow asks the store again
    // and refuses if a bond has appeared in the meantime.
    // The core's button is NOT a trigger - Button.h shares LED_NOTIFICATION_PIN with the
    // notification LED and forces the button state false while the LED is driven, so a press is
    // invisible exactly when it blinks (owner ruling, 2026-10-28).
    if (BleAutoWindowGranted(BleAutoWindowCause, AppBLEBondCount()))
        AppBLEAutoWindow(BleAutoWindowCause);
    BleAutoWindowCause = BleWindowCauseNone;

    // Advertising itself (BleAdvWanted) has one condition and is unchanged by the window or by the
    // bond store; what those decide is who may BOND and who may hold a session, which is answered
    // per link (see onConnect/onAuthenticationComplete). Nothing re-aims here any more: the
    // advertisement carries no filter policy (E66).
    const bool AdvWanted = BleAdvWanted();
    NimBLEAdvertising *Adv = NimBLEDevice::getAdvertising();

    if (!AdvWanted)
    {
        // Off means off: the block switched Enabled off (AppBLESettingsApply already stopped the
        // advertisement; this is the safety net for the state arriving any other way).
        if (BleAdvStateApplied)
        {
            BleAdvStateApplied = false;
            BleAdvRestart = false;
            if (Adv)
                Adv->stop();
            DeviceLog("APPBLE", "advertising stopped: the radio is disabled");
        }
    }
    else if (!BleAdvStateApplied)
    {
        BleAdvStateApplied = true;
        // Rebuild, never drop: the advertisement stays on the air across a window opening or
        // closing, and across a bond appearing or being forgotten.
        BleAdvRestart = true;
    }

    // ==== the session latch ====
    // Every check of "is a link really there" happens here, against the stack's own list, at a
    // cadence (BLE_LATCH_CHECK_MS) rather than every pass: this is what repairs a teardown whose
    // onDisconnect never came, and what keeps a link the stack refuses to terminate from holding
    // the radio for the life of the boot (Blocks/BleLinkLatch.h).
    static uint32_t LatchCheckMs = 0;
    if ((uint32_t)(TimeFromBoot() - LatchCheckMs) >= BLE_LATCH_CHECK_MS)
    {
        LatchCheckMs = TimeFromBoot();
        BleLatchSupervise();
    }

    // A connection that has not become encrypted+bonded by its deadline belongs to a peer that
    // never started - or never completed - pairing. Drop it; a session is only ever established
    // by onAuthenticationComplete. The log names the case, because the two have different causes:
    // with a passkey the phone had a dialog up and nobody typed the code in time, without one the
    // peer simply never paired.
    //
    // The deadline belongs to BleSecureHandle (the link it was armed for), not to the session, so a
    // refused SECOND peer's deadline cannot drop the live session. And the deadline is cleared only
    // AFTER the drop has been requested and handed to the latch's supervisor, which retries it and
    // frees the latch if the stack will not co-operate: clearing it first (which this did) left a
    // drop that failed with nothing left to retry it.
    if (BleSecureDeadlineMs != 0 && (int32_t)(TimeFromBoot() - BleSecureDeadlineMs) >= 0)
    {
        const uint16_t Pending = BleSecureHandle;
        DeviceLog("APPBLE", "dropping handle %u: it did not become secure within %u ms (%s)",
                  (unsigned)Pending, (unsigned)BleSecureBudgetUsedMs,
                  BleSecureLinkMitm ? "passkey set: the code was not entered"
                                    : "just works: pairing never completed");
        BleClearSecureDeadline();
        BleDropLink(Pending, "did not become secure in time");
    }

    // Deferred advertising restart.
    if (!BleConnected && BleOldConnected)
        BleAdvRestart = true; // s_ble_parser was already reset by onDisconnect (host task)
    bool WasConnected = BleOldConnected;
    BleOldConnected = BleConnected;
    if (WasConnected && !BleConnected)
        DeviceLog("APPBLE", "BLE session ended");
    else if (!WasConnected && BleConnected)
        DeviceLog("APPBLE", "BLE session started");

    // Deferred restart after a clean disconnect, and the very first start. An advertisement exists
    // whenever no session is up (BleAdvWanted), so it comes straight back the moment the link drops.
    if (BleAdvRestart && !BleConnected && AdvWanted)
    {
        BleAdvRestart = false;
        StartAppAdvertising();
    }

    // Self-healing watchdog. Rebuilding advertising tears the GATT service
    // registration down and back up; doing that on a timer wedges the host
    // stack after enough cycles, so only act on evidence:
    //  - stack admits the outage (isAdvertising()==false): rebuild promptly;
    //  - stale advertising instance claiming to run with no session ever
    //    arriving: one conservative rebuild as a last resort (long interval).
    // Both only apply while an advertisement is wanted: a radio the block has
    // switched off has nothing on the air to heal.
    static uint32_t IdleSince = 0;
    if (AdvWanted && !BleConnected && !BleOldConnected)
    {
        const bool AdvUp = Adv && Adv->isAdvertising();
        if (IdleSince == 0) IdleSince = TimeFromBoot();
        const uint32_t IdleMs = TimeFromBoot() - IdleSince;
        if (!AdvUp && IdleMs > 3000)
        {
            DeviceLog("APPBLE", "advertising down %u ms, rebuilding", IdleMs);
            IdleSince = TimeFromBoot();
            StartAppAdvertising();
        }
        else if (AdvUp && IdleMs > 120000)
        {
            DeviceLog("APPBLE", "no BLE session for 120 s, forcing adv rebuild");
            IdleSince = TimeFromBoot();
            StartAppAdvertising();
        }
    }
    else
        IdleSince = 0;

    if (!BleConnected)
        return;

    // ---- TX ----
    // No artificial pacing: AppBLETick runs from the ~2 ms app-task loop, which throttles
    // notifications by itself.
    if (!AppInterface.TxRing)
        return;

    // Notification payload must fit the NEGOTIATED ATT MTU: mtu - 3 for the ATT
    // header, minus 2 for our length prefix. With the default MTU of 23 only 18
    // stream bytes fit per notification; larger chunks flow once the app negotiates.
    uint16_t Budget = BleMtu;
    if (Budget < 23)
        Budget = 23;
    if (Budget > 3 + 2 + BLE_CHUNK)
        Budget = 3 + 2 + BLE_CHUNK;
    uint16_t ChunkCap = Budget - 3 - 2;

    static uint8_t Chunk[BLE_CHUNK];   // static: ~1 KB, too big for the task stack slice
    uint16_t Len = AppInterface.TxPeek(Chunk, ChunkCap);
    if (Len == 0)
        return;

    static uint8_t Pkt[2 + BLE_CHUNK]; // only touched here (single app-task context)
    Pkt[0] = (uint8_t)(Len & 0xFF);
    Pkt[1] = (uint8_t)(Len >> 8);
    memcpy(&Pkt[2], Chunk, Len);

    if (BleTx && BleTx->notify(Pkt, (size_t)(2 + Len)))
    {
        AppInterface.TxCommit(Len); // consumed only when the stack accepted the notification
        NotifyFails = 0;
    }
    else
    {
        // backpressure - retry on the next tick without losing data; log only
        // when it persists so a wedged TX path becomes visible in diagnostics
        if (++NotifyFails == 100 || (NotifyFails > 100 && NotifyFails % 500 == 0))
            DeviceLog("APPBLE", "notify backpressure x%u", (unsigned)NotifyFails);
    }
}
