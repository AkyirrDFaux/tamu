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

void AppbleResetSessionBreadcrumbs()
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

class BleServerCallbacks : public NimBLEServerCallbacks
{
    void onConnect(NimBLEServer *PServer, NimBLEConnInfo &ConnInfo) override
    {
        BleMtu = ConnInfo.getMTU();
        BleConnected = true;
        StaticBleRxAssembler.Reset();
        BleParser.Reset();
        // Prefer 2M PHY when the central supports it: halves air time per packet
        // (helps multi-notification transfers); stays at 1M otherwise.
        PServer->updatePhy(ConnInfo.getConnHandle(),
                           BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
        // BlueZ defaults to a long connection interval (~30-60 ms); request a
        // fast one so transactions do not wait multiple connection events.
        // Units: interval 1.25 ms (6..12 -> 7.5..15 ms), timeout 10 ms (400 -> 4 s).
        PServer->updateConnParams(ConnInfo.getConnHandle(), 6, 12, 0, 400);
    }

    void onDisconnect(NimBLEServer *PServer, NimBLEConnInfo &ConnInfo, int Reason) override
    {
        BleConnected = false;
        // Drop any half-received chunk with the session (see BleRxAssembler::reset).
        StaticBleRxAssembler.Reset();
        BleParser.Reset();
        BleMtu = 23;
        AppInterface.TxFlushAll(); // pending responses belong to the dead session
        AppbleResetSessionBreadcrumbs();
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
    SystemNameCString(AdvName);
    bool NameOk = Adv->setName(AdvName);
    bool Started = Adv->start();
    DeviceLog("APPBLE", "adv restart: uuid=%d name=%d started=%d",
              UuidOk, NameOk, Started);
}

void AppBLEInit(const char *DeviceName)
{
    NimBLEDevice::init(DeviceName);
    NimBLEDevice::setMTU(512);

    BleServer = NimBLEDevice::createServer();
    BleServer->setCallbacks(&StaticBleServerCallbacks);

    NimBLEService *Svc = BleServer->createService(BLE_SERVICE_UUID);

    NimBLECharacteristic *Rx = Svc->createCharacteristic(
        BLE_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    Rx->setCallbacks(&StaticBleTxCallbacks);

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
    bool WasConnected = BleOldConnected;
    BleOldConnected = BleConnected;
    if (WasConnected && !BleConnected)
        DeviceLog("APPBLE", "BLE session ended");
    else if (!WasConnected && BleConnected)
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
