//ESP32-C3 Warning: Stopping serial freezes board if DTR + RTS enabled

#include <BLEDevice.h>
#include <BLE2902.h>

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic;

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

bool deviceConnected = false;
bool oldDeviceConnected = false;
ByteArray BufferIn;

#define BTCHUNK 64

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

        if (rxValue.length() > 0){
            BufferIn = BufferIn << ByteArray(rxValue.c_str(), rxValue.length());
            Serial.println(BufferIn.ToHex());
        }
            
    }
};

class ChirpClass
{
public:
    void Begin(String Name);
    void Send(String Data);
    void Send(const ByteArray &Input);
    void Communicate();
    BaseClass *DecodePart(int32_t *Start);
};

void ChirpClass::Begin(String Name)
{
    Serial.setTimeout(1);
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
};

void ChirpClass::Send(String Data)
{
    Serial.println(Data);
};

void ChirpClass::Send(const ByteArray &Input)
{
    if (deviceConnected)
    {
        ByteArray Buffer = Input.AddLength();
        //Serial.println(Buffer.ToHex());

        for (int32_t Index = 0; Index < Buffer.Length; Index += BTCHUNK)
        {
            pTxCharacteristic->setValue((uint8_t *)Buffer.Array + Index, min((size_t)(Buffer.Length - Index), (size_t)BTCHUNK));
            pTxCharacteristic->notify();
            delay(5);
        }
    }
};

void ChirpClass::Communicate()
{
    String Input = "";
    Input += Serial.readStringUntil('\n');

    // Serial

    // BT

    // disconnecting
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
};
