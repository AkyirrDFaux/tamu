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
            BufferIn = BufferIn << ByteArray(rxValue.c_str(), rxValue.length());
    }
};

class ChirpClass
{
public:
    // ByteArray BufferOut;
    void Begin(String Name);
    // void Send(ByteArray Data);
    void Send(String Data);
    void Send(const Message &Input);
    void SendNow(const Message &Input);
    void Communicate();
    // void Decode();
    BaseClass *DecodePart(int32_t *Start);
};

void ChirpClass::Begin(String Name)
{
    Serial.setTimeout(1);
    BLEDevice::init(std::string(Name.c_str(),Name.length()));
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

/*
void ChirpClass::Send(ByteArray Data)
{
    BufferOut.Append(Data.Array, Data.Length);
};
*/
void ChirpClass::Send(String Data)
{
    Serial.println(Data);
};

void ChirpClass::Send(const Message &Input)
{
    Serial.println(Input.AsText());
    if (deviceConnected)
    {
        ByteArray Length = ByteArray(Input.Length());
        pTxCharacteristic->setValue((uint8_t *)Length.Array, (size_t)Length.Length);
        pTxCharacteristic->notify();
        delay(5);

        for (int32_t Index = 0; Index < Input.Segments.Length; Input.Segments.Iterate(&Index))
        {
            pTxCharacteristic->setValue((uint8_t *)Input.Segments[Index]->Array, (size_t)Input.Segments[Index]->Length);
            pTxCharacteristic->notify();
            delay(5);
        }
    }
};

void ChirpClass::SendNow(const Message &Input)
{
    Serial.println(Input.AsText());
    // delay(1000);
};

void ChirpClass::Communicate()
{
    String Input = "";
    Input += Serial.readStringUntil('\n');

    // Serial
    if (Input.length() > 0)
    {
        Message *M = Deserialize(Input);
        if (M != nullptr)
        {
            Send(M->AsText()); // Debug

            Message *Response = M->Run();
            if (Response != nullptr)
            {
                Send(*Response);
                delete Response;
            }

            delete M;
        }
    }

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

    if (BufferIn.Length > sizeof(uint32_t) && BufferIn.Length >= BufferIn.As<uint32_t>() + sizeof(uint32_t))
    {
        ByteArray MsgData = BufferIn.SubArray(sizeof(uint32_t), BufferIn.As<uint32_t>());
        Message M = Message(MsgData);
        Send(M.AsText()); // Debug

        Message *Response = M.Run();
        if (Response != nullptr)
        {
            Send(*Response);
            delete Response;
        }

        BufferIn = BufferIn.SubArray(BufferIn.As<uint32_t>() + sizeof(uint32_t));
    }
};
