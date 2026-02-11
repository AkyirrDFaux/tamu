// ESP32-C3 Warning: Stopping serial freezes board if DTR + RTS enabled
ByteArray BufferIn;
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0

#include "NimBLEDevice.h"

NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pTxCharacteristic = nullptr;

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

bool deviceConnected = false;
bool oldDeviceConnected = false;

#define BTCHUNK 512

class MyServerCallbacks : public NimBLEServerCallbacks
{
    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override
    {
        ESP_LOGI("CHIRP", "CONNECTED: %s\n", connInfo.getAddress().toString().c_str());
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
        ESP_LOGI("CHIRP", "CALLBACK TRIGGERED\n");
        std::string rxValue = pCharacteristic->getValue();
        if (rxValue.length() > 0)
        {
            ESP_LOG_BUFFER_HEX("CHIRP", rxValue.data(), rxValue.length());

            BufferIn = BufferIn << ByteArray(rxValue.data(), rxValue.length());
        }
    }
} staticRxCallbacks;
#elif defined BOARD_Valu_v2_0
uint32_t LastSend = 0;
#endif

class ChirpClass
{
public:
    void Begin(Text Name);
    void SendNow(const ByteArray &Input);
    void Send(const ByteArray &Input);
    void Communicate();
    BaseClass *DecodePart(int32_t *Start);
};

void ChirpClass::Begin(Text Name)
{
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
    // Initialize NimBLE
    NimBLEDevice::init(std::string(Name.Data, Name.Length));
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

    ESP_LOGI("CHIRP", "BLE Started");
#elif defined BOARD_Valu_v2_0
    LastSend = HW::Now();
#endif
};

void ChirpClass::SendNow(const ByteArray &Input)
{
    HW::USB_Send(Input.CreateMessage());
};

void ChirpClass::Send(const ByteArray &Input)
{
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
    if (deviceConnected)
    {
        ByteArray Buffer = Input.CreateMessage();
        for (int32_t Index = 0; Index < Buffer.Length; Index += BTCHUNK)
        {
            size_t chunkLen = std::min((size_t)(Buffer.Length - Index), (size_t)BTCHUNK);
            pTxCharacteristic->setValue((uint8_t *)Buffer.Array + Index, chunkLen);
            pTxCharacteristic->notify();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    // Also send via USB for cross-compatibility
#endif
    SendNow(Input);

};

void ChirpClass::Communicate()
{
#if defined BOARD_Valu_v2_0
    tud_task();
#endif
    ByteArray Input = HW::USB_Read();

    if (Input.Length > 0)
    {
        NotificationBlink(1, 20);

        BufferIn = BufferIn << Input;
        ByteArray Message = BufferIn.ExtractMessage();
        if (Message.Length > 0)
            Run(Message);
    }

// BT

// disconnecting
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
    /*// 1. Handle USB CDC Data
    char usb_buf[128];
    int len = usb_serial_jtag_read_bytes(usb_buf, sizeof(usb_buf), 0); // Non-blocking
    if (len > 0)
    {
        BufferIn = BufferIn << ByteArray(usb_buf, len);
    }*/

    // 2. Handle Bluetooth Connection State
    if (!deviceConnected && oldDeviceConnected)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        pServer->getAdvertising()->start();
        ESP_LOGI("CHIRP", "Restarted Advertising");
        oldDeviceConnected = false;
    }
    if (deviceConnected && !oldDeviceConnected)
    {
        oldDeviceConnected = true;
    }

    // 3. Process the Protocol Buffer
    ByteArray Message = BufferIn.ExtractMessage();
    if (Message.Length > 0)
    {
        Run(Message);
    }
#endif
};
