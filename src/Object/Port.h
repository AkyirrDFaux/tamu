class PortTypeClass
{
public:
    uint32_t Values = Ports::None;
    PortTypeClass() {};
    PortTypeClass(uint32_t Input) { Values = Input; };
    void operator=(uint32_t Other) { Values = Other; };
    void operator+=(uint32_t Other) { Values |= Other; };
    void operator-=(uint32_t Other) { Values &= ~Other; };
    bool operator==(Ports::Ports Other) { return Values & Other; };
    bool operator!=(Ports::Ports Other) { return !(Values & Other); };
};

class PortClass : public BaseClass
{
public:
    Pin PortPin;
    enum Value
    {
        PortType,
        DriverType
    };

    void *DriverObj = nullptr;

    PortClass(Pin NewPin, PortTypeClass NewPortType = Ports::None);
    ~PortClass();

    void AddModule(BaseClass *Object, int32_t Index = -1);
    void RemoveModule(BaseClass *Object);

    static void AddModuleBridge(BaseClass *Base, BaseClass *Object, int32_t Index) { static_cast<PortClass *>(Base)->AddModule(Object, Index); }
    static void RemoveModuleBridge(BaseClass *Base, BaseClass *Object) { static_cast<PortClass *>(Base)->RemoveModule(Object); }
    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = BaseClass::DefaultRun,
        .AddModule = PortClass::AddModuleBridge,
        .RemoveModule = PortClass::RemoveModuleBridge};

    int32_t CountLED();
    void AssignLED(Pin Pin);
};

constexpr VTable PortClass::Table;

PortClass::PortClass(Pin NewPin, PortTypeClass NewPortType) : BaseClass(&Table) // Created by Board
{
    PortPin = NewPin;
    ValueSet(NewPortType);
    ValueSet(Drivers::None);

    Type = ObjectTypes::Port;
    Name = "Port";
    Flags = Flags::Auto;
};

PortClass::~PortClass()
{
    for (uint32_t Index = 0; Index < Modules.Length; Index++)
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
    I2C *I2CDriver = nullptr;
    I2CClass();

    void AddModule(BaseClass *Object, int32_t Index = -1);
    void RemoveModule(BaseClass *Object);

    static void AddModuleBridge(BaseClass *Base, BaseClass *Object, int32_t Index) { static_cast<I2CClass *>(Base)->AddModule(Object, Index); }
    static void RemoveModuleBridge(BaseClass *Base, BaseClass *Object) { static_cast<I2CClass *>(Base)->RemoveModule(Object); }
    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = BaseClass::DefaultRun,
        .AddModule = I2CClass::AddModuleBridge,
        .RemoveModule = I2CClass::RemoveModuleBridge};

    void StartModule(BaseClass *Object);
    void StopModule(BaseClass *Object);
};

constexpr VTable I2CClass::Table;

I2CClass::I2CClass() : BaseClass(&Table)
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
    // HardwareSerial *UART = nullptr;
    UARTClass();

    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = BaseClass::DefaultRun,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};

    // Unfinished
};

constexpr VTable UARTClass::Table;

UARTClass::UARTClass() : BaseClass(&Table)
{
    Type = ObjectTypes::UART;
    Name = "UART Bus";
}