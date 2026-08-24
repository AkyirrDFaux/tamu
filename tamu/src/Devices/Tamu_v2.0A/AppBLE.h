#pragma once

// BLE link for the App Interface: Nordic UART style GATT service (same UUIDs as the
// previous Chirp implementation), notify (device -> app) + write (app -> device).
// Each transfer carries a uint16 LE length prefix followed by that many packet-stream
// bytes (Docs/Services/App Interface.md BLE packet layout).
//
// Requires AppUSB.h to be included first (shares CommLed + WireStreamParser).

#include "NimBLEDevice.h"

#define BLE_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_RX_CHAR_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // app writes here
#define BLE_TX_CHAR_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // device notifies

#define BLE_CHUNK 180          // stream bytes per notification (+2 byte length prefix);
                               // fits the Android default ATT MTU of 185
#define BLE_PACE_MS 20         // min spacing between notifications (Chirp heritage)

static NimBLEServer *BleServer = nullptr;
static NimBLECharacteristic *BleTx = nullptr;
static volatile bool BleConnected = false;
static bool BleOldConnected = false;   // for deferred advertising restart
static bool BleAdvRestart = false;     // advertising must be restarted
static volatile uint16_t BleMtu = 23;  // negotiated ATT MTU (ATT default)
static uint32_t LastBleSend = 0;

static WireStreamParser s_ble_parser; // ApplicationTask-context only

// Reassembles the length-prefixed BLE chunks (uint16 LE + stream). BlueZ may
// deliver characteristic writes fragmented or coalesced arbitrarily, so this
// cannot assume one onWrite == one complete chunk.
struct BleRxAssembler
{
    uint16_t need      = 0;   // stream bytes of the current chunk
    uint16_t got       = 0;   // stream bytes accumulated so far
    bool     haveLen   = false;
    uint8_t  lenBuf[2] = {0, 0};
    uint32_t malformed = 0;   // diagnostics: chunks dropped for zero length

    // Feeds one raw byte; returns true when a stream byte should go into the RX ring.
    inline bool feed(uint8_t b, uint8_t *out)
    {
        if (!haveLen)
        {
            lenBuf[got++] = b;
            if (got == 2)
            {
                need   = (uint16_t)(lenBuf[0] | (lenBuf[1] << 8));
                got    = 0;
                haveLen= true;
                if (need == 0) { malformed++; DeviceLog("APPBLE", "Zero-length BLE chunk dropped"); haveLen = false; }
            }
            return false;
        }

        *out = b;
        if (++got >= need)
        {
            got = 0;
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
    }

    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override
    {
        BleConnected = false;
        BleMtu = 23;
        AppTxFlushAll(); // pending responses belong to the dead session
    }

    void onMTUChange(uint16_t MTU, NimBLEConnInfo &connInfo) override
    {
        BleMtu = MTU;
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
        std::string v = pCharacteristic->getValue();
        size_t n = v.length();

        for (size_t i = 0; i < n; i++)
        {
            uint8_t b;
            if (!staticBleRxAssembler.feed((uint8_t)v[i], &b))
                continue;
            // Assembled stream bytes go straight into the wire-stream parser;
            // complete frames are queued for the pump (no intermediate buffer).
            if (s_ble_parser.Feed(b))
            {
                AppInterfaceEnqueue(s_ble_parser.frame);
                s_ble_parser.Reset();
            }
            else if (s_ble_parser.full)
            {
                DeviceLog("APPBLE", "Rejected malformed app frame (CRC)");
                s_ble_parser.Reset(); // CRC failure: resync
            }
        }

        CommLed(true);
    }
} staticBleTxCallbacks;


// Rebuilds and starts the advertisement from scratch. Used for the initial
// start AND every post-session restart: restarting the previous instance can
// leave an active-but-invisible advertisement (observed on hardware).
static void StartAppAdvertising()
{
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (!adv)
        return;

    adv->stop();
    adv->clearData();
    adv->removeServices();
    bool uuidOk = adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->enableScanResponse(true); // must precede setName (see ordering note)
    bool nameOk = adv->setName(DeviceName);
    bool started = adv->start();
    DeviceLog("APPBLE", "adv restart: uuid=%d name=%d started=%d",
              uuidOk, nameOk, started);
}

void AppBLEInit(const char *DeviceName)
{
    NimBLEDevice::init(DeviceName);
    NimBLEDevice::setMTU(256);

    BleServer = NimBLEDevice::createServer();
    BleServer->setCallbacks(&staticBleServerCallbacks);

    NimBLEService *svc = BleServer->createService(BLE_SERVICE_UUID);

    NimBLECharacteristic *rx = svc->createCharacteristic(
        BLE_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(&staticBleTxCallbacks);

    BleTx = svc->createCharacteristic(BLE_TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);

    svc->start();

    StartAppAdvertising();
}

bool AppBLEActive()
{
    return BleConnected;
}

// Pumped from ApplicationTask: drains the RX ring into the packet parser and pushes
// paced notifications out while TX data is pending. Also restarts advertising after
// a disconnect (deferred out of the BLE callback context, retried until it succeeds).
void AppBLETick()
{
    // Deferred advertising restart.
    if (!BleConnected && BleOldConnected)
    {
        BleAdvRestart = true;
        s_ble_parser.Reset();
    }
    bool wasConnected = BleOldConnected;
    BleOldConnected = BleConnected;
    if (wasConnected && !BleConnected)
        DeviceLog("APPBLE", "BLE session ended");
    else if (!wasConnected && BleConnected)
        DeviceLog("APPBLE", "BLE session started");

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

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
    static uint32_t idle_since = 0;
    if (!BleConnected && !BleOldConnected)
    {
        const bool advUp = adv && adv->isAdvertising();
        if (idle_since == 0) idle_since = TimeFromBoot();
        const uint32_t idleMs = TimeFromBoot() - idle_since;
        if (!advUp && idleMs > 3000)
        {
            DeviceLog("APPBLE", "advertising down %u ms, rebuilding", idleMs);
            idle_since = TimeFromBoot();
            StartAppAdvertising();
        }
        else if (advUp && idleMs > 120000)
        {
            DeviceLog("APPBLE", "no BLE session for 120 s, forcing adv rebuild");
            idle_since = TimeFromBoot();
            StartAppAdvertising();
        }
    }
    else
        idle_since = 0;

    if (!BleConnected)
        return;

    // ---- TX (paced) ----
    uint32_t now = TimeFromBoot();
    if (now - LastBleSend < BLE_PACE_MS || !AppTxRing)
        return;

    // Notification payload must fit the NEGOTIATED ATT MTU: mtu - 3 for the ATT
    // header, minus 2 for our length prefix. With the default MTU of 23 only 18
    // stream bytes fit per notification; larger chunks flow once the app negotiates.
    uint16_t budget = BleMtu;
    if (budget < 23)
        budget = 23;
    if (budget > 3 + 2 + BLE_CHUNK)
        budget = 3 + 2 + BLE_CHUNK;
    uint16_t chunkCap = budget - 3 - 2;

    uint8_t chunk[BLE_CHUNK];
    uint16_t n = AppTxPeek(chunk, chunkCap);
    if (n == 0)
        return;

    uint8_t pkt[2 + BLE_CHUNK];
    pkt[0] = (uint8_t)(n & 0xFF);
    pkt[1] = (uint8_t)(n >> 8);
    memcpy(&pkt[2], chunk, n);

    if (BleTx && BleTx->notify(pkt, (size_t)(2 + n)))
    {
        AppTxCommit(n); // consumed only when the stack accepted the notification
        CommLed(true);
        LastBleSend = now;
    }
    // else: backpressure - retry on the next tick without losing data
}
