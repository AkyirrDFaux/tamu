// ESP32-C3 Warning: Stopping serial freezes board if DTR + RTS enabled
ByteArray BufferIn;
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include <BLEDevice.h>
#include <BLE2902.h>

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic;

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

bool deviceConnected = false;
bool oldDeviceConnected = false;

#define BTCHUNK 512

class MyServerCallbacks : public BLEServerCallbacks
{
    void onConnect(BLEServer *pServer)
    {
        deviceConnected = true;
    };

    void onDisconnect(BLEServer *pServer)
    {
        deviceConnected = false;
    }
};

class MyCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        std::string rxValue = pCharacteristic->getValue();

        if (rxValue.length() > 0)
        {
            BufferIn = BufferIn << ByteArray(rxValue.c_str(), rxValue.length());
            Serial.println(BufferIn.ToHex());
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
    BLEDevice::init(std::string(Name.c_str(), Name.length()));
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_UUID);
    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY);

    pTxCharacteristic->addDescriptor(new BLE2902());
    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE);

    pRxCharacteristic->setCallbacks(new MyCallbacks());

    // Start the service
    pService->start();

    // Start advertising
    pServer->getAdvertising()->start();
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
        // Serial.println(Buffer.ToHex());

        for (int32_t Index = 0; Index < Buffer.Length; Index += BTCHUNK)
        {
            pTxCharacteristic->setValue((uint8_t *)Buffer.Array + Index, min((size_t)(Buffer.Length - Index), (size_t)BTCHUNK));
            pTxCharacteristic->notify();
            delay(5);
        }
    }
#else
    SendNow(Input);
#endif
};

void ChirpClass::Communicate()
{
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
// BT

// disconnecting
#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
    if (!deviceConnected && oldDeviceConnected)
    {
        delay(500);                  // give the bluetooth stack the chance to get things ready
        pServer->startAdvertising(); // restart advertising
        Serial.println("start advertising");
        oldDeviceConnected = deviceConnected;
    }
    // connecting
    if (deviceConnected && !oldDeviceConnected)
        oldDeviceConnected = deviceConnected;

    ByteArray Message = BufferIn.ExtractMessage();

    if (Message.Length > 0)
        Run(Message);
#endif
};
