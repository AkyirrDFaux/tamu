// ESP32-C3 Warning: Stopping serial freezes board if DTR + RTS enabled
FlexArray BufferUSBIn;
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
uint32_t LastSendBT = 0;
FlexArray BufferBLEIn;
FlexArray BufferBLEOut;
#include "NimBLEDevice.h"

NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pTxCharacteristic = nullptr;

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

bool deviceConnected = false;
bool oldDeviceConnected = false;

#define BTCHUNK 500
#define BTDELAY 20

class MyServerCallbacks : public NimBLEServerCallbacks
{
    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override
    {
        // ESP_LOGI("CHIRP", "CONNECTED: %s\n", connInfo.getAddress().toString().c_str());
        deviceConnected = true;
    }
    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override
    {
        deviceConnected = false;
    }
} staticServerCallbacks;

class MyCallbacks : public NimBLECharacteristicCallbacks
{
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override
    {
        std::string rxValue = pCharacteristic->getValue();
        if (rxValue.length() > 0)
        {
            // ESP_LOG_BUFFER_HEX("CHIRP IN", rxValue.data(), rxValue.length());

            BufferBLEIn.Append(rxValue.data(), rxValue.length());
            // ESP_LOGI("CHIRP IN", "Now %d bytes", BufferBLEIn.Length);
        }
    }
} staticRxCallbacks;
#endif

class ChirpClass
{
public:
    void Begin(Text Name);
    void Send(const FlexArray &Input);
    void Communicate();
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
    void SendBTChunk();
#endif
};

void ChirpClass::Begin(Text Name)
{
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
    // Initialize NimBLE
    NimBLEDevice::init(std::string(Name.Data, Name.Length));
    NimBLEDevice::setMTU(512);
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(&staticServerCallbacks);

    // Explicitly create UUID objects to verify parsing
    NimBLEUUID svcUUID(SERVICE_UUID);
    NimBLEUUID rxUUID(CHARACTERISTIC_UUID_RX);
    NimBLEUUID txUUID(CHARACTERISTIC_UUID_TX);

    NimBLEService *pService = pServer->createService(svcUUID);

    // Define RX
    NimBLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
        rxUUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    pRxCharacteristic->setCallbacks(&staticRxCallbacks);

    // Define TX
    pTxCharacteristic = pService->createCharacteristic(
        txUUID,
        NIMBLE_PROPERTY::NOTIFY);

    // Start Service
    pService->start();

    // Start Advertising
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(svcUUID);
    pAdvertising->start();

    // ESP_LOGI("CHIRP", "BLE Started");
#endif
};

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
void ChirpClass::SendBTChunk()
{
    // 1. Data & Timing Check
    if (BufferBLEOut.Length == 0 || HW::Now() - LastSendBT < BTDELAY)
        return;

    // 2. MTU/Chunk Safety
    // If setMTU is 512, safe payload is 509. Let's use 500 to be safe.
    size_t chunkLen = (BufferBLEOut.Length > BTCHUNK) ? BTCHUNK : BufferBLEOut.Length;

    // 3. Set Value and Notify
    pTxCharacteristic->setValue((uint8_t *)BufferBLEOut.Array, chunkLen);

    // 4. Only clear the data we actually SENT
    if (pTxCharacteristic->notify())
    {
        BufferBLEOut.Consume(chunkLen); // This moves the remaining data to the front
        LastSendBT = HW::Now();
    }
}
#endif

void ChirpClass::Send(const FlexArray &Input)
{
    FlexArray Buffer = Input.PrependLength();
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
    if (deviceConnected)
    {
        BufferBLEOut += Buffer;
        SendBTChunk();
    }
#endif
    HW::USB_Send(Buffer);
};

void ChirpClass::Communicate()
{
#if defined BOARD_Valu_v2_0
    HW::tud_task();
#endif
    if (HW::USB_Read(BufferUSBIn) > 0)
    {
        HW::ModeOutput(LED_NOTIFICATION_PIN);
        HW::Low(LED_NOTIFICATION_PIN); // On

        FlexArray Message = BufferUSBIn.ExtractByLength();
        if (Message.Length > 0)
            Run(Message);

        HW::High(LED_NOTIFICATION_PIN); // Off
        HW::ModeInput(LED_NOTIFICATION_PIN);
    }

// BT

// disconnecting
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
    // 2. Handle Bluetooth Connection State
    if (!deviceConnected && oldDeviceConnected)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        pServer->getAdvertising()->start();
        // ESP_LOGI("CHIRP", "Restarted Advertising");
        oldDeviceConnected = false;
    }
    if (deviceConnected && !oldDeviceConnected)
    {
        oldDeviceConnected = true;
    }

    if (deviceConnected)
    {
        SendBTChunk();

        // 3. Process the Protocol Buffer
        // ESP_LOGI("CHIRP", "Extracting BT");
        FlexArray Message = BufferBLEIn.ExtractByLength();
        // ESP_LOGI("CHIRP", "Extracted BT");
        if (Message.Length > 0)
        {
            HW::ModeOutput(LED_NOTIFICATION_PIN);
            HW::Low(LED_NOTIFICATION_PIN); // On
            Run(Message);
            HW::High(LED_NOTIFICATION_PIN); // Off
            HW::ModeInput(LED_NOTIFICATION_PIN);
        }
    }
#endif
};
