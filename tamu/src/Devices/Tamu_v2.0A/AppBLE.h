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
#define BLE_RX_RING_SIZE 1024

static NimBLEServer *BleServer = nullptr;
static NimBLECharacteristic *BleTx = nullptr;
static volatile bool BleConnected = false;
static bool BleOldConnected = false;   // for deferred advertising restart
static bool BleAdvRestart = false;     // advertising must be restarted
static volatile uint16_t BleMtu = 23;  // negotiated ATT MTU (ATT default)
static uint32_t LastBleSend = 0;

static uint8_t BleRxRing[BLE_RX_RING_SIZE];
static volatile uint16_t BleRxHead = 0, BleRxTail = 0;
static portMUX_TYPE BleRxMux = portMUX_INITIALIZER_UNLOCKED;

static WireStreamParser s_ble_parser; // ApplicationTask-context only

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
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override
    {
        std::string v = pCharacteristic->getValue();
        size_t n = v.length();
        if (n == 0)
            return;

        portENTER_CRITICAL(&BleRxMux);
        for (size_t i = 0; i < n; i++)
        {
            uint16_t used = (uint16_t)(BleRxHead - BleRxTail);
            if (used >= BLE_RX_RING_SIZE - 1)
                break; // ring full: drop the rest of this write
            BleRxRing[BleRxHead % BLE_RX_RING_SIZE] = (uint8_t)v[i];
            BleRxHead = (uint16_t)(BleRxHead + 1);
        }
        portEXIT_CRITICAL(&BleRxMux);

        CommLed(true);
    }
} staticBleTxCallbacks;

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

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->enableScanResponse(true);
    adv->start();
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
    BleOldConnected = BleConnected;
    if (BleAdvRestart && !BleConnected)
    {
        NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
        if (adv && adv->start())
            BleAdvRestart = false;
    }

    if (!BleConnected)
        return;

    // ---- RX ----
    uint8_t buf[128];
    for (;;)
    {
        uint16_t n = 0;
        portENTER_CRITICAL(&BleRxMux);
        uint16_t used = (uint16_t)(BleRxHead - BleRxTail);
        if (used > sizeof(buf))
            used = sizeof(buf);
        for (n = 0; n < used; n++)
            buf[n] = BleRxRing[BleRxTail % BLE_RX_RING_SIZE];
        portEXIT_CRITICAL(&BleRxMux);

        if (n == 0)
            break;
        BleRxTail = (uint16_t)(BleRxTail + n);
        CommLed(false); // burst done

        for (uint16_t i = 0; i < n; i++)
        {
            if (s_ble_parser.Feed(buf[i]))
            {
                AppInterfaceEnqueue(s_ble_parser.frame);
                s_ble_parser.Reset();
            }
            else if (s_ble_parser.full)
            {
                s_ble_parser.Reset(); // CRC failure: resync
            }
        }
    }

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
