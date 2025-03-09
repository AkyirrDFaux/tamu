class PortClass : public Variable<Ports>
{
public:
    Variable<uint8_t> Pin = Variable<uint8_t>(0, RandomID, Flags::Auto);
    Variable<Drivers> DriverType = Variable<Drivers>(Drivers::None, RandomID, Flags::Auto);
    void *Driver = nullptr;
    PortAttachClass *Attached = nullptr;

    PortClass(u_int8_t NewPin, Ports NewPortType = Ports::None);
    ~PortClass();
    void Setup();
    void StopDriver();
    bool Attach(PortAttachClass *Object);
    void Detach();
};

PortClass::PortClass(u_int8_t NewPin, Ports NewPortType) : Variable(NewPortType) // Created by Board
{
    Pin = NewPin;

    Type = Types::Port;
    Name = "Port";
    Flags = Flags::Auto;

    AddModule(&Pin);
    Pin.Name = "Pin";
    AddModule(&DriverType);
    DriverType.Name = "Driver";
};

void PortClass::StopDriver()
{
    switch (DriverType)
    {
    case Drivers::LED:
        EndLED((CRGB*)Driver);
        break;
    }
    Driver = nullptr;
    DriverType = Drivers::None;
};

PortClass::~PortClass()
{
    Detach();
}

class PortAttachClass : public Variable<int32_t>
{
public:
    PortClass *Port = nullptr;
    BaseClass *Key(); // Only one key allowed!

    void Setup();
    bool Check(Drivers RequiredDriver);
    PortAttachClass(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~PortAttachClass();
};

PortAttachClass::PortAttachClass(bool New, IDClass ID, FlagClass Flags) : Variable(-1, ID, Flags)
{
    Type = Types::PortAttach;
    Name = "Port Attachment";
};

PortAttachClass::~PortAttachClass()
{
    *Data = -1;
    Setup();
};

BaseClass *PortAttachClass::Key()
{
    for (int32_t Index = 0; Index < Objects.Allocated; Index++)
    {
        if (Objects.IsValid(Index) == false)
            continue;
            
        int32_t Found = Objects[Index]->Modules.Find(this);
        if (Found != -1)
            return Objects[Index];
    }
    return nullptr;
};

bool PortClass::Attach(PortAttachClass *Object)
{
    if (Attached == nullptr)
    {
        Attached = Object;
        Setup();
        return true;
    }
    return false;
};

void PortClass::Detach()
{
    if (Attached != nullptr)
    {
        Attached->Port = nullptr;
        Attached = nullptr;
    }
    Setup();
};