// ESP32-C3 Warning: Stopping serial freezes board if DTR + RTS enabled
ByteArray BufferIn;
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include "esp_bt.h"
#include "esp_nimble_hci.h"
extern "C"
{
    // Add these alongside your other bridges in main.cpp
    int ble_gattc_notify_custom(uint16_t conn_handle, uint16_t char_val_handle, struct os_mbuf *om) { return 0; }
    int ble_gattc_indicate_custom(uint16_t conn_handle, uint16_t char_val_handle, struct os_mbuf *om) { return 0; }
}
#include "NimBLEDevice.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h" // For USB communication

static const char *TAG = "CHIRP";

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
    void onConnect(NimBLEServer *pServer) override
    {
        deviceConnected = true;
    }
    void onDisconnect(NimBLEServer *pServer) override
    {
        deviceConnected = false;
    }
};

class MyCallbacks : public NimBLECharacteristicCallbacks
{
    void onWrite(NimBLECharacteristic *pCharacteristic) override
    {
        std::string rxValue = pCharacteristic->getValue();
        if (rxValue.length() > 0)
        {
            BufferIn = BufferIn << ByteArray(rxValue.data(), rxValue.length());
            // Replace Serial.println with ESP_LOGI
            // ESP_LOGI(TAG, "BLE RX: %s", BufferIn.ToHex());
        }
    }
};
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
    // 1. Initialize USB-Serial-JTAG for Chirp communication
    usb_serial_jtag_driver_config_t usb_config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&usb_config);

    // 2. Initialize NimBLE
    NimBLEDevice::init(std::string(Name.Data, Name.Length));
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    NimBLEService *pService = pServer->createService(SERVICE_UUID);

    // TX Characteristic (Notify)
    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        NIMBLE_PROPERTY::NOTIFY);

    // RX Characteristic (Write)
    NimBLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        NIMBLE_PROPERTY::WRITE);
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    pService->start();
    pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
    pServer->getAdvertising()->start();
#elif defined BOARD_Valu_v2_0
    LastSend = HW::Now();
#endif
};

void ChirpClass::SendNow(const ByteArray &Input)
{
#if defined BOARD_Valu_v2_0
    HW::USB_Send(Input.CreateMessage());
#endif
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
    usb_serial_jtag_write_bytes(Input.Array, Input.Length, pdMS_TO_TICKS(100));
#else
    SendNow(Input);
#endif
};

void ChirpClass::Communicate()
{
#if defined BOARD_Valu_v2_0
    tud_task();
    /*if (HW::Now() - LastSend > 1000)
    {
        LastSend = HW::Now();
        const char *Hello = "Hello!\n";
        HW::USB_Send(ByteArray(Hello, strlen(Hello)));
    }*/

    ByteArray Input = HW::USB_Read();

    if (Input.Length > 0)
    {
        NotificationBlink(1, 20);

        BufferIn = BufferIn << Input;
        ByteArray Message = BufferIn.ExtractMessage();
        if (Message.Length > 0)
            Run(Message);
    }
#endif
// BT

// disconnecting
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
    // 1. Handle USB CDC Data
    char usb_buf[128];
    int len = usb_serial_jtag_read_bytes(usb_buf, sizeof(usb_buf), 0); // Non-blocking
    if (len > 0)
    {
        BufferIn = BufferIn << ByteArray(usb_buf, len);
    }

    // 2. Handle Bluetooth Connection State
    if (!deviceConnected && oldDeviceConnected)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        pServer->getAdvertising()->start();
        ESP_LOGI(TAG, "Restarted Advertising");
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
