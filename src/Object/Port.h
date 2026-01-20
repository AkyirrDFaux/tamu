class PortClass : public BaseClass
{
public:
    enum Value
    {
        PortType,
        Pin,
        DriverType
    };

    void *DriverObj = nullptr;

    PortClass(uint8_t NewPin, Ports NewPortType = Ports::None);
    ~PortClass();

    void AddModule(BaseClass *Object, int32_t Index = -1);
    void RemoveModule(BaseClass *Object);
};

PortClass::PortClass(uint8_t NewPin, Ports NewPortType) : BaseClass() // Created by Board
{
    Values.Add(NewPortType);
    Values.Add(NewPin);
    Values.Add(Drivers::None);

    Type = ObjectTypes::Port;
    Name = "Port";
    Flags = Flags::Auto;
};

PortClass::~PortClass()
{
    for (int32_t Index = 0; Index < Modules.Length; Index++)
        RemoveModule(Modules[Index]);
}

class I2CClass : public BaseClass
{
public:
    enum Value
    {
        SDA,
        SCL
    };
    TwoWire *I2C = nullptr;
    I2CClass();

    void AddModule(BaseClass *Object, int32_t Index = -1);
    void RemoveModule(BaseClass *Object);
};

I2CClass::I2CClass()
{
    Type = ObjectTypes::I2C;
    Name = "I2C Bus";
}

class UARTClass : public BaseClass
{
public:
    enum Value
    {
        TX,
        RX
    };
    HardwareSerial *UART = nullptr;
    UARTClass();
    //Unfinished
};

UARTClass::UARTClass()
{
    Type = ObjectTypes::UART;
    Name = "UART Bus";
}