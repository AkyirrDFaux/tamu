#pragma once

// BLE link for the App Interface: Nordic UART style GATT service (same UUIDs as the
// previous Chirp implementation), notify (device -> app) + write (app -> device).
// Each transfer carries a uint16 LE length prefix followed by that many packet-stream
// bytes (Docs/Services/App Interface.md BLE packet layout).
//
// Requires AppUSB.h to be included first (shares WireStreamParser).

#include "NimBLEDevice.h"
#include "Blocks/BleAdmissionPolicy.h"
#include "Blocks/BleSecurityPolicy.h"
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
#define BLE_WINDOW_MS 60000           // connectable window opened by the block's Action field
#define BLE_AUTO_WINDOW_MS 300000     // ... and opened by itself for 5 min while no bond is stored
// A link that has not completed pairing by its deadline is dropped (AppBLETick). The budget itself
// is policy-shaped and lives with the policy (Blocks/BleSecurityPolicy.h, BleSecureBudgetMs): Just
// Works waits for one tap, a passkey waits for a person to read a prompt and type six digits. Both
// fit inside the connectable window the pairing was started in, which is why the passkey figure has
// to stay below BLE_WINDOW_MS - pinned here so the two numbers cannot drift apart again.
static_assert(BleSecureBudgetPasskeyMs < BLE_WINDOW_MS,
              "a passkey pairing must fit inside the window that opened it");

static volatile bool BleSecured = false;      // current link encrypted, bonded (and authenticated)
static uint32_t BleWindowEndMs = 0;           // 0 = the connectable window is closed
static uint32_t BleSecureDeadlineMs = 0;      // 0 = no unsecured link pending
static uint32_t BleSecureBudgetUsedMs = 0;    // the budget the pending link was granted (for the log)
static bool BleSecureLinkMitm = false;        // ... and whether it was granted the passkey budget
static uint16_t BleConnHandle = 0xFFFF;
static bool BleAdvStateApplied = false;       // the advertisement matches BleAdvWanted()
// The bond store as it was when the current link came up, and whether the peer that finished the
// security procedure is one of those bonds. The session gate asks the second question (E66;
// Blocks/BleAdmissionPolicy.h), so the answer has to be taken before the link can change the store.
static NimBLEAddress BleLinkBonds[CONFIG_BT_NIMBLE_MAX_BONDS];
static uint8_t BleLinkBondCount = 0;
// True while the current link may not create a bond: no window is open and the store is not empty,
// so a pairing on this link is a stranger's and must not leave a bond behind.
static bool BleLinkBondingClosed = false;
// The recovery window is opened once per "bond store is empty" episode: set when it has been
// opened, cleared again as soon as a bond exists (or the radio is switched off and back on), so it
// is not a permanent advertisement.
static bool BleAutoWindowDone = false;

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



// Fed and reset only from the NimBLE host task (BleTxCallbacks::onWrite and the
// connect/disconnect callbacks), never from AppBLETick - see the framer race note there.
static WireStreamParser BleParser;

// Reassembles the length-prefixed BLE chunks (uint16 LE + stream). BlueZ may
// deliver characteristic writes fragmented or coalesced arbitrarily, so this
// cannot assume one onWrite == one complete chunk.
struct BleRxAssembler
{
    uint16_t Need      = 0;   // stream bytes of the current chunk
    uint16_t Got       = 0;   // stream bytes accumulated so far
    bool     HaveLen   = false;
    uint8_t  LenBuf[2] = {0, 0};

    // Clears the in-flight chunk state.
    // Called on connect/disconnect: a session that ends mid-chunk would otherwise leave
    // haveLen/got set, and the next session's first bytes would be read as payload instead of
    // a length prefix - the frame then never completes and nothing is ever dispatched, with no
    // CRC rejection to show for it either.
    inline void Reset()
    {
        Need    = 0;
        Got     = 0;
        HaveLen = false;
    }

    // Feeds one raw byte; returns true when a stream byte should go into the RX ring.
    inline bool Feed(uint8_t Byte, uint8_t *Out)
    {
        if (!HaveLen)
        {
            LenBuf[Got++] = Byte;
            if (Got == 2)
            {
                Need   = (uint16_t)(LenBuf[0] | (LenBuf[1] << 8));
                Got    = 0;
                HaveLen= true;
                if (Need == 0) { DeviceLog("APPBLE", "Zero-length BLE chunk dropped"); HaveLen = false; }
            }
            return false;
        }

        *Out = Byte;
        if (++Got >= Need)
        {
            Got = 0;
            HaveLen = false;
        }
        return true;
    }
} StaticBleRxAssembler;

// ===== Connectable window =====
//
// A window is the PAIRING state. While one is open any phone may connect and pair (up to the three
// bonds the store holds). A window is opened by the `BLE Settings` block's Action field (USB), and
// it opens BY ITSELF for BLE_AUTO_WINDOW_MS whenever the bond store is empty - first use, or
// straight after an Action = 1 forget - so a headless device is easy to find and pair to. The
// core's button is not a trigger: Button.h shares LED_NOTIFICATION_PIN with the notification LED and
// forces the button state false while the LED is driven, so a press is invisible exactly when it
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
// it is that a stranger's CONNECT_REQ is answered before the link is refused at the security
// procedure instead of being ignored by the controller. See the advertising rule below and
// Issues.md E66.
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

// Opens (or extends) the connectable window for the manual period. Called from the block's
// Action field, in the app task. Returns false when the radio cannot be discovered at all.
bool AppBLEOpenWindow()
{
    if (!BleRadioUp || !BleEnabled)
    {
        DeviceLog("APPBLE", "window refused: the radio is disabled");
        return false;
    }
    BleWindowEndMs = TimeFromBoot() + BLE_WINDOW_MS;
    DeviceLog("APPBLE", "connectable window open for %u ms", (unsigned)BLE_WINDOW_MS);
    return true;
}

// The recovery window: with no bond stored, the device makes itself discoverable for five minutes
// without being asked. Driven by the bond store, never by a button (see above).
bool AppBLEAutoWindow()
{
    if (!BleRadioUp || !BleEnabled || AppBLEBondCount() != 0)
        return false;
    const uint32_t NewEnd = TimeFromBoot() + BLE_AUTO_WINDOW_MS;
    if (BleWindowEndMs != 0 && (int32_t)(NewEnd - BleWindowEndMs) <= 0)
        return true; // a longer window is already open
    BleWindowEndMs = NewEnd;
    DeviceLog("APPBLE", "no bonds stored: window open for %u ms", (unsigned)BLE_AUTO_WINDOW_MS);
    return true;
}

// ===== Bond store read-out / erasure (the block's Bond fields and Action=1) =====
//
// Order and eviction (the card's rules, settled against this build's NimBLE):
//   - The store keeps entries in the order they were added (ble_store_config_peer_secs is an
//     append array, re-sorted by bond_count at boot), so key 0 is the OLDEST bond and
//     getBondedAddress(i) walks oldest first - what the block's Bond fields promise.
//   - Capacity is CONFIG_BT_NIMBLE_MAX_BONDS = 3 (static_assert'd against the field's key count).
//     When a fourth phone pairs, the config store answers BLE_HS_ESTORE_CAP, and the default
//     store-status handler that esp-nimble-cpp installs (ble_store_util_status_rr via
//     NimBLEDeviceCallbacks) calls ble_gap_unpair_oldest_peer() and retries: the OLDEST bond is
//     evicted, so the phone that paired first is the one that has to pair again. Nothing is
//     silently dropped - the store is full, one entry goes.
//   - Because that eviction and the new bond land inside ONE store-status callback, the size of
//     the store reads 3 before and 3 after while its content changed entirely. The block's Bond
//     fields therefore cannot be refreshed on the count alone (they used to be, and kept listing
//     the evicted phone): every completed bond and every forget bumps BleBondGeneration, and
//     BleSettingsSync re-formats on a change of it. The count and a slow content check in the
//     glue are the safety net for a store that changes without either event.

// "The bond store's content may differ from what the block last formatted". Bumped from the
// NimBLE host task (onAuthenticationComplete) and the app task (AppBLEForgetAllBonds); read by the
// main loop, so volatile - a 32-bit aligned word, which the C3 updates atomically.
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

// Writes bond `Index`'s identity address ("AA:BB:CC:DD:EE:FF") NUL-terminated into `Out`.
// The NimBLE bond store keeps no peer name, so the address is the whole entry.
bool AppBLEBondText(int Index, char *Out, size_t Len)
{
    if (!Out || Len == 0)
        return false;
    Out[0] = '\0';
    if (!BleRadioUp || Index < 0 || Index >= NimBLEDevice::getNumBonds())
        return false;
    const std::string Text = NimBLEDevice::getBondedAddress(Index).toString();
    snprintf(Out, Len, "%s", Text.c_str());
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
// NimBLEAddress compares the raw type byte, and the two sides do not use the same one: the bond
// store keeps 0/1 (public/random) while a peer the controller resolved is reported as 2/3
// (public/random IDENTITY). BleSameIdentity compares the six bytes and the public/random axis.
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
bool AppBLEForgetAllBonds()
{
    if (!BleServer)
        return false;
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
    }
    else
    {
        DeviceLog("APPBLE", "bond forget left %d bond(s): epoch not stamped, the next radio start retries",
                  After);
    }

    DeviceLog("APPBLE", "bonds forgotten: %d -> %d (ok=%d)", Before, After, Ok);
    AppBLEBondStoreChanged(); // the block's Bond texts follow in the next loop pass
    return Ok;
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
void AppBLEEnforceBondPolicy()
{
    // The bond store's API needs the host; before createServer() there is nothing to compare
    // against, and the radio start calls this again once the store exists.
    if (!BleServer)
        return;

    if (!BleBondEpochKnown)
        BleBondEpochKnown = BleBondEpochLoad(BleBondEpoch);

    const char *Now = StaticVol.BleSet.Passkey;
    const bool Unknown = !BleBondEpochKnown;
    if (!Unknown && memcmp(BleBondEpoch, Now, BLE_PASSKEY_LEN) == 0)
        return;

    // An unstamped store predates this rule, and the only build that could have written it had no
    // passkey at all - its bonds are Just Works bonds. Under a passkey they can never authenticate,
    // so they go; under Just Works they are exactly what the current policy asks for, so they are
    // kept and merely stamped (no re-pairing for every device that upgrades).
    const int Bonds = NimBLEDevice::getNumBonds();
    const bool Invalidated = !Unknown || BlePasskeyEnabled(Now);
    if (Bonds > 0 && Invalidated)
    {
        // The phone that paired under the old code cannot produce an authenticated link, and its
        // stored key would keep it connecting - it has to pair again inside a window.
        DeviceLog("APPBLE", "passkey changed since the bonds were made: forgetting %d bond(s)", Bonds);
        AppBLEForgetAllBonds(); // re-stamps the epoch only if the store really came up empty
        return;
    }
    AppBLEBondEpochStamp(); // nothing to revoke: just record which code the store is under now
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

    // A Just Works session is unauthenticated by definition: once a passkey is set it no longer
    // meets the policy, so it is dropped rather than kept on the strength of the old decision.
    if (Policy.Mitm && BleSecured && BleConnHandle != 0xFFFF && BleServer)
    {
        DeviceLog("APPBLE", "dropping the Just Works session: a passkey is now set");
        BleServer->disconnect(BleConnHandle);
    }

    // The links that were paired under the previous code are worthless now - and would keep the
    // phone connecting with a bond it can never authenticate. Done after the policy is in force, so
    // the next connection inside a window pairs fresh and gets the prompt.
    AppBLEEnforceBondPolicy();
}

class BleServerCallbacks : public NimBLEServerCallbacks
{
    // A connection is NOT an app session. It becomes one only when the link is encrypted,
    // bonded - and, with a passkey set, authenticated - which is what onAuthenticationComplete
    // reports. Until then BleConnected stays false: no notification is sent and the GATT flags
    // refuse every read and write at the ATT layer.
    void onConnect(NimBLEServer *PServer, NimBLEConnInfo &ConnInfo) override
    {
        // One live connection at a time. The Tamu protocol has a single receive stream and one
        // session state, so a second peer would interleave with the first app's traffic rather
        // than get its own session. The controller allows three connections
        // (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3 because that is what the stack is built for), so
        // the refusal has to be here: the second phone is disconnected immediately and its GATT
        // access never reaches a handler. The phone sees a normal "disconnected by the device",
        // and the log names the reason.
        if (BleConnHandle != 0xFFFF && BleConnHandle != ConnInfo.getConnHandle())
        {
            DeviceLog("APPBLE", "refused a second peer: a link is already live (handle %u)",
                      (unsigned)BleConnHandle);
            PServer->disconnect(ConnInfo.getConnHandle());
            return;
        }

        BleConnHandle = ConnInfo.getConnHandle();
        BleMtu = 23;
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
            PServer->disconnect(ConnInfo.getConnHandle());
            return;
        }

        // Who may HOLD a link is decided by a bond the peer can prove, not by the address it
        // arrives on: with a bond stored and no window open, only a peer that already had a key can
        // come up encrypted AND bonded (E66, Blocks/BleAdmissionPolicy.h). A peer that arrives from
        // a PUBLIC address has no private address in the way, so the store can be asked here and a
        // stranger is refused now rather than after a pairing attempt. A private one may be the
        // owner behind an address this device cannot resolve for the filter (E66), so that case is
        // left to the security procedure and judged in onAuthenticationComplete.
        if (!AppBLEWindowOpen() && AppBLEBondCount() > 0 &&
            !BlePeerWasBonded(ConnInfo.getIdAddress()) &&
            (ConnInfo.getIdAddress().getType() & 0x01) == 0)
        {
            DeviceLog("APPBLE", "refused a stranger: the pairing window is closed");
            PServer->disconnect(ConnInfo.getConnHandle());
            return;
        }

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
        BleSecureBudgetUsedMs = BleSecureBudgetMs(Policy);
        BleSecureLinkMitm = Policy.Mitm;
        BleSecureDeadlineMs = TimeFromBoot() + BleSecureBudgetUsedMs;
    }

    void onDisconnect(NimBLEServer *PServer, NimBLEConnInfo &ConnInfo, int Reason) override
    {
        BleConnected = false;
        BleSecured = false;
        BleConnHandle = 0xFFFF;
        BleSecureDeadlineMs = 0;
        // Drop any half-received chunk with the session (see BleRxAssembler::reset).
        StaticBleRxAssembler.Reset();
        BleParser.Reset();
        BleMtu = 23;
        AppInterface.TxFlushAll(); // pending responses belong to the dead session
        AppbleResetSessionBreadcrumbs();
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
            if (BleServer)
                BleServer->disconnect(ConnInfo.getConnHandle());
            return;
        }

        // With a passkey set, encryption without authentication means the pairing skipped the
        // code, so the link is refused even though encryption succeeded.
        if (Policy.Mitm && !ConnInfo.isAuthenticated())
        {
            DeviceLog("APPBLE", "unauthenticated link refused: a passkey is set");
            BleSecured = false;
            BleConnected = false;
            if (BleServer)
                BleServer->disconnect(ConnInfo.getConnHandle());
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
            if (BleServer)
                BleServer->disconnect(ConnInfo.getConnHandle());
            return;
        }

        BleSecured = true;
        BleConnected = true;
        BleMtu = ConnInfo.getMTU();
        BleSecureDeadlineMs = 0;
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

        DeviceLog("APPBLE", "link secured: enc=%u bonded=%u auth=%u bonds=%d bits=%u",
                  ConnInfo.isEncrypted(), ConnInfo.isBonded(), ConnInfo.isAuthenticated(),
                  NimBLEDevice::getNumBonds(), ConnInfo.getSecKeySize());

        // A bond exists that did not before - including the case where this pairing overflowed
        // the store and the stack evicted the oldest entry inside the same callback, which leaves
        // getNumBonds() unchanged. Tell the block to re-read the entries rather than the size.
        AppBLEBondStoreChanged();

        // The store is now under the code this bond was just made with. A reconnect only compares
        // (AppBLEBondEpochStamp is idempotent), so the partition is not written per connection.
        AppBLEBondEpochStamp();
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
            uint8_t Byte;
            if (!StaticBleRxAssembler.Feed((uint8_t)Value[i], &Byte))
                continue;
            // Assembled stream bytes go straight into the wire-stream parser;
            // complete frames are queued for the pump (no intermediate buffer).
            if (BleParser.Feed(Byte))
            {
                AppInterface.Enqueue(BleParser.Frame, AppLinkSource::BLE);
                BleParser.Reset();
            }
            else if (BleParser.Full)
            {
                DeviceLog("APPBLE", "Rejected malformed app frame (CRC)");
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
// session gate instead (BleSessionAllowed in onAuthenticationComplete).
//
// The trade-off, stated rather than hidden: a stranger's CONNECT_REQ is now answered, and the link
// is refused a moment later when it cannot prove a bond. The advertisement and its scan response
// were already unfiltered, so nothing new is visible; what is lost is that the controller no longer
// drops the request in the link layer.

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
    DeviceLog("APPBLE", "adv restart: uuid=%d name=%d started=%d filter=none",
              UuidOk, NameOk, Started);
}

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
        DeviceLog("APPBLE", "radio start failed");
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
        DeviceLog("APPBLE", "createServer failed");
        return false;
    }
    BleServer->setCallbacks(&StaticBleServerCallbacks);

    NimBLEService *Svc = BleServer->createService(BLE_SERVICE_UUID);

    // Every characteristic permission requires encryption (the flags below carry the *_ENC bits),
    // plus *_AUTHEN when a passkey is set: the ATT server refuses an unencrypted or
    // unauthenticated peer at the attribute, not in application code.
    NimBLECharacteristic *Rx = Svc->createCharacteristic(BLE_RX_CHAR_UUID, Policy.RxFlags);
    Rx->setCallbacks(&StaticBleTxCallbacks);

    BleTx = Svc->createCharacteristic(BLE_TX_CHAR_UUID, Policy.TxFlags);

    // (NimBLEService::start() is a no-op / deprecated: services start with the server.)

    // Revoke the bonds the current passkey invalidates before anything can use them: a code
    // changed while the radio was down (Enabled = false is a USB-only write, so the store could not
    // be touched then) is applied here.
    AppBLEEnforceBondPolicy();

    const int Bonds = NimBLEDevice::getNumBonds();

    memcpy(BleAppliedName, Name, SYSTEM_NAME_LEN + 1);
    BleRadioUp = true;
    BleEnabled = true;
    // Do NOT start advertising here: AppBLETick owns that, and its first pass starts one straight
    // away - advertising does not wait for a bond or for a window.
    BleAdvRestart = false;
    BleWindowEndMs = 0;
    BleAdvStateApplied = false;

    DeviceLog("APPBLE", "radio up: name='%s' mode=%s bonds=%d",
              BleAppliedName, Policy.Mitm ? "passkey" : "just works", Bonds);
    return true;
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
        AppBLERadioStart(); // first enable, or the boot of a device that had it on
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
            BleSecureDeadlineMs = 0;
            BleAdvRestart = false;
            BleAutoWindowDone = false; // a later re-enable gets its recovery window again
            // The advertisement was stopped here, so the tick must not look for a transition.
            BleAdvStateApplied = false;
            if (BleServer && BleConnHandle != 0xFFFF)
                BleServer->disconnect(BleConnHandle);
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
        NimBLEDevice::setDeviceName(Name);
        memcpy(BleAppliedName, Name, SYSTEM_NAME_LEN + 1);
        DeviceLog("APPBLE", "advertised name -> '%s'", BleAppliedName);
        // Rebuild rather than mutate in place: the same reason as StartAppAdvertising's note.
        // A bonded device carries the new name on the air outside a window too, so this follows
        // the advertising rule, not the window.
        if (BleAdvWanted())
        {
            NimBLEAdvertising *Adv = NimBLEDevice::getAdvertising();
            if (Adv)
                Adv->stop();
            BleAdvRestart = true;
        }
    }
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

    // Recovery window: while the bond store is empty the device opens the window by itself for
    // BLE_AUTO_WINDOW_MS (first use, or straight after Action=1 forgot the bonds), so a headless
    // device is easy to find and pair to. Pairing does not depend on it - an empty store already
    // admits any peer (BleSessionAllowed) - but it is what the block's Window field reports and
    // what makes the device obviously discoverable in that state. Opened once per empty episode,
    // not kept open.
    // The core's button is NOT a trigger - Button.h shares LED_NOTIFICATION_PIN with the
    // notification LED and forces the button state false while the LED is driven, so a press is
    // invisible exactly when it blinks (owner ruling, 2026-10-28).
    if (AppBLEBondCount() == 0)
    {
        if (!BleAutoWindowDone)
        {
            BleAutoWindowDone = true; // one window per empty episode, not a permanent advert
            AppBLEAutoWindow();
        }
    }
    else
        BleAutoWindowDone = false;

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

    // A connection that has not become encrypted+bonded by its deadline belongs to a peer that
    // never started - or never completed - pairing. Drop it; a session is only ever established
    // by onAuthenticationComplete. The log names the case, because the two have different causes:
    // with a passkey the phone had a dialog up and nobody typed the code in time, without one the
    // peer simply never paired.
    if (!BleSecured && BleSecureDeadlineMs != 0 &&
        (int32_t)(TimeFromBoot() - BleSecureDeadlineMs) >= 0)
    {
        BleSecureDeadlineMs = 0;
        DeviceLog("APPBLE", "dropping a link that did not become secure within %u ms (%s)",
                  (unsigned)BleSecureBudgetUsedMs,
                  BleSecureLinkMitm ? "passkey set: the code was not entered"
                                    : "just works: pairing never completed");
        if (BleServer && BleConnHandle != 0xFFFF)
            BleServer->disconnect(BleConnHandle);
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
