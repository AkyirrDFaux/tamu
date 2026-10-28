#pragma once

// BLE link for the App Interface: Nordic UART style GATT service (same UUIDs as the
// previous Chirp implementation), notify (device -> app) + write (app -> device).
// Each transfer carries a uint16 LE length prefix followed by that many packet-stream
// bytes (Docs/Services/App Interface.md BLE packet layout).
//
// Requires AppUSB.h to be included first (shares WireStreamParser).

#include "NimBLEDevice.h"
#include "Blocks/BleSecurityPolicy.h"

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
#define BLE_SECURE_DEADLINE_MS 5000   // a link that has not become secure by then is dropped

static volatile bool BleSecured = false;      // current link encrypted, bonded (and authenticated)
static uint32_t BleWindowEndMs = 0;           // 0 = the connectable window is closed
static uint32_t BleSecureDeadlineMs = 0;      // 0 = no unsecured link pending
static uint16_t BleConnHandle = 0xFFFF;
static bool BleWindowApplied = false;         // advertising/filter state matches the window
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
// The device is discoverable and connectable only while a window is open: it advertises nothing
// at all outside one, so a scan finds no Tamu and even a bonded phone cannot start a connection.
// A window is opened by the `BLE Settings` block's Action field (USB), and it opens BY ITSELF for
// BLE_AUTO_WINDOW_MS whenever the bond store is empty - first use, or straight after an
// Action = 1 forget - so a headless device is always recoverable. The core's button is not a
// trigger: Button.h shares LED_NOTIFICATION_PIN with the notification LED and forces the button
// state false while the LED is driven, so a press is invisible exactly when it blinks.
// A radio that the block has disabled has no window at all.

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
//     silently dropped - the store is full, one entry goes, and the block's Bond count follows
//     getNumBonds() on the next main-loop pass (BleSettingsSync), so the app sees the new count
//     within one ~2 ms iteration.

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

// Drops every bond and the whitelist that mirrors it: a phone that was bonded is a stranger
// afterwards and has to pair again inside a window.
bool AppBLEForgetAllBonds()
{
    if (!BleRadioUp)
        return false;
    const int Before = NimBLEDevice::getNumBonds();
    const bool Ok = NimBLEDevice::deleteAllBonds();

    // The whitelist is our own list, not the bond store's: remove what we added (through the
    // API, so the local mirror and the controller stay in step).
    while (NimBLEDevice::getWhiteListCount() > 0)
    {
        const NimBLEAddress Addr = NimBLEDevice::getWhiteListAddress(0);
        if (!NimBLEDevice::whiteListRemove(Addr))
            break;
    }

    DeviceLog("APPBLE", "bonds forgotten: %d -> %d (ok=%d)", Before, NimBLEDevice::getNumBonds(), Ok);
    return Ok;
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

        // The block can switch the radio off at any moment; a connection that raced the switch is
        // dropped rather than served.
        if (!BleEnabled)
        {
            DeviceLog("APPBLE", "refused: the radio is disabled");
            PServer->disconnect(ConnInfo.getConnHandle());
            return;
        }

        // A peer the bond store does not know can only be here to pair, and pairing is allowed
        // only inside the connectable window. One we do know is kept: it encrypts with the key
        // it stored right after connecting.
        if (!ConnInfo.isBonded() && !AppBLEWindowOpen())
        {
            DeviceLog("APPBLE", "refused an unpaired peer: the pairing window is closed");
            PServer->disconnect(ConnInfo.getConnHandle());
            return;
        }

        // Ask for security now instead of waiting for the first refused attribute access, so the
        // phone shows its pair dialog (and, with a passkey set, asks for the code) immediately.
        const int Rc = ble_gap_security_initiate(ConnInfo.getConnHandle());
        if (Rc != 0)
            DeviceLog("APPBLE", "security initiate failed: rc=%d", Rc);
        BleSecureDeadlineMs = TimeFromBoot() + BLE_SECURE_DEADLINE_MS;
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
            DeviceLog("APPBLE", "pairing failed (enc=%u bonded=%u): dropping the link",
                      ConnInfo.isEncrypted(), ConnInfo.isBonded());
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

        // A completed bond: remember the IDENTITY address. The phone connects from a rotating
        // private address, while the identity address is what the bond store keeps, so a
        // whitelist entry on the identity address survives the rotation. whiteListAdd() returns
        // true when the address is already there, false only when the controller refused it.
        if (!NimBLEDevice::whiteListAdd(ConnInfo.getIdAddress()))
            DeviceLog("APPBLE", "whitelist add failed for %s",
                      ConnInfo.getIdAddress().toString().c_str());

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
    // While the window is open any peer may connect - that is what the window is for. The
    // whitelist-only filter is the state applied when it is closed (ApplyConnectFilter).
    Adv->setScanFilter(false, false);
    bool Started = Adv->start();
    DeviceLog("APPBLE", "adv restart: uuid=%d name=%d started=%d",
              UuidOk, NameOk, Started);
}

// Connect filter for the advertising instance: whitelist-only connect requests while the window
// is closed, any peer while it is open.
//
// Note on the card's `setScanFilterMode`: in esp-nimble-cpp 2.5.0 that call (NimBLEDevice's) is
// the *scan duplicate cache* filter type and must be made before init - it does not restrict who
// may connect. The controller-side connect filter is the advertising filter policy below.
static void ApplyConnectFilter(bool WindowOpen)
{
    NimBLEAdvertising *Adv = NimBLEDevice::getAdvertising();
    if (!Adv)
        return;
    Adv->setScanFilter(!WindowOpen, !WindowOpen);
    DeviceLog("APPBLE", "connect filter: whitelist-only=%d", !WindowOpen);
}

// Starts the radio: the NimBLE host, the pairing policy, the GATT table (built from that policy)
// and the whitelist taken from the bond store. Called at boot when the block's Enabled is true,
// and later if it is switched on. Idempotent per boot: BleRadioUp guards it.
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

    // The bonds already in the store are the whitelist: a bonded phone's identity address has to
    // be there from boot, or a reconnect after a reboot would find an empty filter. NimBLE does
    // not restore the list itself - the bond store does, not the controller's whitelist.
    const int Bonds = NimBLEDevice::getNumBonds();
    for (int i = 0; i < Bonds; i++)
        NimBLEDevice::whiteListAdd(NimBLEDevice::getBondedAddress(i));

    memcpy(BleAppliedName, Name, SYSTEM_NAME_LEN + 1);
    BleRadioUp = true;
    BleEnabled = true;
    // Do NOT start advertising here, and do not start it at boot at all: the device is only
    // discoverable while a connectable window is open (the button or the block's Action field).
    // AppBLETick owns that transition.
    BleAdvRestart = false;
    BleWindowEndMs = 0;
    ApplyConnectFilter(false);
    BleWindowApplied = false;

    DeviceLog("APPBLE", "radio up: name='%s' mode=%s bonds=%d whitelist=%u",
              BleAppliedName, Policy.Mitm ? "passkey" : "just works", Bonds,
              (unsigned)NimBLEDevice::getWhiteListCount());
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
        if (AppBLEWindowOpen())
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
    // It closes by itself; everything about discoverability hangs off it.
    if (BleWindowEndMs != 0 && (int32_t)(TimeFromBoot() - BleWindowEndMs) >= 0)
    {
        BleWindowEndMs = 0;
        DeviceLog("APPBLE", "connectable window closed");
    }

    // Recovery window: while the bond store is empty the device opens the window by itself for
    // BLE_AUTO_WINDOW_MS (first use, or straight after Action=1 forgot the bonds), so a headless
    // device can always be paired to. It is opened once per empty episode, not kept open: when it
    // expires the device goes quiet again until a bond exists or the block asks for a window.
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

    const bool WindowOpen = AppBLEWindowOpen();
    if (WindowOpen != BleWindowApplied)
    {
        BleWindowApplied = WindowOpen;
        ApplyConnectFilter(WindowOpen);
        if (WindowOpen)
        {
            BleAdvRestart = true;
        }
        else
        {
            // Outside the window the device advertises nothing at all: no scan result, and no
            // connection request from even a bonded phone (it cannot address an advert that is
            // not on the air).
            NimBLEAdvertising *ClosedAdv = NimBLEDevice::getAdvertising();
            if (ClosedAdv)
                ClosedAdv->stop();
            BleAdvRestart = false;
        }
    }

    // A connection that has not become encrypted+bonded by its deadline belongs to a peer that
    // never started - or never completed - pairing. Drop it; a session is only ever established
    // by onAuthenticationComplete.
    if (!BleSecured && BleSecureDeadlineMs != 0 &&
        (int32_t)(TimeFromBoot() - BleSecureDeadlineMs) >= 0)
    {
        BleSecureDeadlineMs = 0;
        DeviceLog("APPBLE", "dropping a link that did not become secure");
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

    NimBLEAdvertising *Adv = NimBLEDevice::getAdvertising();

    // Deferred restart after a clean disconnect - only while the window is still open, so a
    // session that ends after it closed leaves nothing on the air.
    if (BleAdvRestart && !BleConnected && WindowOpen)
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
    // Both only apply inside an open window: outside one there is deliberately nothing to heal.
    static uint32_t IdleSince = 0;
    if (WindowOpen && !BleConnected && !BleOldConnected)
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
