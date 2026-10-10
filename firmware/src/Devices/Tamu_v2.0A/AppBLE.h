#pragma once

// BLE link for the App Interface: Nordic UART style GATT service (same UUIDs as the
// previous Chirp implementation), notify (device -> app) + write (app -> device).
// Each transfer carries a uint16 LE length prefix followed by that many packet-stream
// bytes (Docs/Services/App Interface.md BLE packet layout).
//
// Requires AppUSB.h to be included first (shares WireStreamParser).

#include "NimBLEDevice.h"

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

void appbleResetSessionBreadcrumbs()
{
    NotifyFails = 0;
    LoggedFirstWrite = false;
}
static volatile bool BleConnected = false;
static bool BleOldConnected = false;   // for deferred advertising restart
static bool BleAdvRestart = false;     // advertising must be restarted
static volatile uint16_t BleMtu = 23;  // negotiated ATT MTU (ATT default)

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
    bool     haveLen   = false;
    uint8_t  lenBuf[2] = {0, 0};

    // Clears the in-flight chunk state.
    // Called on connect/disconnect: a session that ends mid-chunk would otherwise leave
    // haveLen/got set, and the next session's first bytes would be read as payload instead of
    // a length prefix - the frame then never completes and nothing is ever dispatched, with no
    // CRC rejection to show for it either.
    inline void Reset()
    {
        Need    = 0;
        Got     = 0;
        haveLen = false;
    }

    // Feeds one raw byte; returns true when a stream byte should go into the RX ring.
    inline bool Feed(uint8_t B, uint8_t *Out)
    {
        if (!haveLen)
        {
            lenBuf[Got++] = B;
            if (Got == 2)
            {
                Need   = (uint16_t)(lenBuf[0] | (lenBuf[1] << 8));
                Got    = 0;
                haveLen= true;
                if (Need == 0) { DeviceLog("APPBLE", "Zero-length BLE chunk dropped"); haveLen = false; }
            }
            return false;
        }

        *Out = B;
        if (++Got >= Need)
        {
            Got = 0;
            haveLen = false;
        }
        return true;
    }
} staticBleRxAssembler;

class BleServerCallbacks : public NimBLEServerCallbacks
{
    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override
    {
        BleMtu = connInfo.getMTU();
        BleConnected = true;
        staticBleRxAssembler.Reset();
        BleParser.Reset();
        // Prefer 2M PHY when the central supports it: halves air time per packet
        // (helps multi-notification transfers); stays at 1M otherwise.
        pServer->updatePhy(connInfo.getConnHandle(),
                           BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
        // BlueZ defaults to a long connection interval (~30-60 ms); request a
        // fast one so transactions do not wait multiple connection events.
        // Units: interval 1.25 ms (6..12 -> 7.5..15 ms), timeout 10 ms (400 -> 4 s).
        pServer->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 400);
    }

    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int Reason) override
    {
        BleConnected = false;
        // Drop any half-received chunk with the session (see BleRxAssembler::reset).
        staticBleRxAssembler.Reset();
        BleParser.Reset();
        BleMtu = 23;
        AppInterface.TxFlushAll(); // pending responses belong to the dead session
        extern void appbleResetSessionBreadcrumbs();
        appbleResetSessionBreadcrumbs();
    }

    void onMTUChange(uint16_t MTU, NimBLEConnInfo &connInfo) override
    {
        BleMtu = MTU;
    }

    void onPhyUpdate(NimBLEConnInfo &connInfo, uint8_t txPhy, uint8_t rxPhy) override
    {
        DeviceLog("APPBLE", "phy updated: tx=%u rx=%u", txPhy, rxPhy);
    }

    void onConnParamsUpdate(NimBLEConnInfo &connInfo) override
    {
        // Diagnostic breadcrumb: confirms the fast-interval request was accepted
        // by the central (7..15 ms keeps transaction round trips short).
        DeviceLog("APPBLE", "conn params: itvl=%u.%02u ms mtu=%u",
                  connInfo.getConnInterval() * 125 / 100,
                  connInfo.getConnInterval() * 125 % 100,
                  connInfo.getMTU());
    }
} staticBleServerCallbacks;

class BleTxCallbacks : public NimBLECharacteristicCallbacks
{
    // App -> device bytes arrive from the NimBLE host task: buffer only, no processing.
    // Writes carry length-prefixed chunks (uint16 LE + stream, Docs/Services/App
    // Interface.md) but BlueZ may deliver them fragmented or coalesced arbitrarily,
    // so every byte goes through the reassembler instead of assuming write==chunk.
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override
    {
        std::string V = pCharacteristic->getValue();
        size_t N = V.length();
        if (!LoggedFirstWrite)
        {
            LoggedFirstWrite = true;
            DeviceLog("APPBLE", "first write: %u Bytes", (unsigned)N);
        }

        for (size_t I = 0; I < N; I++)
        {
            uint8_t B;
            if (!staticBleRxAssembler.Feed((uint8_t)V[I], &B))
                continue;
            // Assembled stream bytes go straight into the wire-stream parser;
            // complete frames are queued for the pump (no intermediate buffer).
            if (BleParser.Feed(B))
            {
                AppInterface.Enqueue(BleParser.Frame);
                BleParser.Reset();
            }
            else if (BleParser.Full)
            {
                DeviceLog("APPBLE", "Rejected malformed app frame (CRC)");
                BleParser.Reset(); // CRC failure: resync
            }
        }

    }
} staticBleTxCallbacks;


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
    bool uuidOk = Adv->addServiceUUID(BLE_SERVICE_UUID);
    Adv->enableScanResponse(true); // must precede setName (see ordering note)
    // The System Name is a fixed 16-char field (NUL-padded since B10, no terminator); BLE wants a
    // NUL-terminated C string, so copy it trimmed.
    char advName[SYSTEM_NAME_LEN + 1];
    uint8_t N = SYSTEM_NAME_LEN;
    while (N > 0 && staticPer.System.Name[N - 1] == ' ') N--;
    memcpy(advName, staticPer.System.Name, N);
    advName[N] = '\0';
    bool nameOk = Adv->setName(advName);
    bool Started = Adv->start();
    DeviceLog("APPBLE", "adv restart: uuid=%d name=%d started=%d",
              uuidOk, nameOk, Started);
}

void AppBLEInit(const char *DeviceName)
{
    NimBLEDevice::init(DeviceName);
    NimBLEDevice::setMTU(512);

    BleServer = NimBLEDevice::createServer();
    BleServer->setCallbacks(&staticBleServerCallbacks);

    NimBLEService *Svc = BleServer->createService(BLE_SERVICE_UUID);

    NimBLECharacteristic *Rx = Svc->createCharacteristic(
        BLE_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    Rx->setCallbacks(&staticBleTxCallbacks);

    BleTx = Svc->createCharacteristic(BLE_TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);

    // (NimBLEService::start() is a no-op / deprecated: services start with the server.)

    // Do NOT start advertising here: the controller may not have synced with
    // the radio yet, and start() then succeeds while nothing reaches the air.
    // Defer to AppBLETick (runs once the host is fully up).
    BleAdvRestart = true;
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

    // Deferred advertising restart.
    if (!BleConnected && BleOldConnected)
        BleAdvRestart = true; // s_ble_parser was already reset by onDisconnect (host task)
    bool wasConnected = BleOldConnected;
    BleOldConnected = BleConnected;
    if (wasConnected && !BleConnected)
        DeviceLog("APPBLE", "BLE session ended");
    else if (!wasConnected && BleConnected)
        DeviceLog("APPBLE", "BLE session started");

    NimBLEAdvertising *Adv = NimBLEDevice::getAdvertising();

    // Deferred restart after a clean disconnect.
    if (BleAdvRestart && !BleConnected)
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
    static uint32_t IdleSince = 0;
    if (!BleConnected && !BleOldConnected)
    {
        const bool advUp = Adv && Adv->isAdvertising();
        if (IdleSince == 0) IdleSince = TimeFromBoot();
        const uint32_t idleMs = TimeFromBoot() - IdleSince;
        if (!advUp && idleMs > 3000)
        {
            DeviceLog("APPBLE", "advertising down %u ms, rebuilding", idleMs);
            IdleSince = TimeFromBoot();
            StartAppAdvertising();
        }
        else if (advUp && idleMs > 120000)
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
    uint16_t chunkCap = Budget - 3 - 2;

    static uint8_t Chunk[BLE_CHUNK];   // static: ~1 KB, too big for the task stack slice
    uint16_t N = AppInterface.TxPeek(Chunk, chunkCap);
    if (N == 0)
        return;

    static uint8_t Pkt[2 + BLE_CHUNK]; // only touched here (single app-task context)
    Pkt[0] = (uint8_t)(N & 0xFF);
    Pkt[1] = (uint8_t)(N >> 8);
    memcpy(&Pkt[2], Chunk, N);

    if (BleTx && BleTx->notify(Pkt, (size_t)(2 + N)))
    {
        AppInterface.TxCommit(N); // consumed only when the stack accepted the notification
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
