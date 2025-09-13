class PortClass : public Variable<Ports>
{
public:
    Variable<uint8_t> Pin = Variable<uint8_t>(0, RandomID, Flags::Auto);
    Variable<Drivers> DriverType = Variable<Drivers>(Drivers::None, RandomID, Flags::Auto);
    void *DriverObj = nullptr;
    ObjectList<> Attached = ObjectList<>();

    PortClass(uint8_t NewPin, Ports NewPortType = Ports::None);
    ~PortClass();

    bool CheckDriver(Drivers Driver);
    void StopDriver();
    Drivers StartDriver();

    void Disconnect(BaseClass *ThatObject);
    CRGB *GetLED(BaseClass *ThatObject);
    TwoWire *GetI2C(BaseClass *ThatObject);
    ESP32PWM *GetPWM(BaseClass *ThatObject);
    Servo *GetServo(BaseClass *ThatObject);
};

PortClass::PortClass(uint8_t NewPin, Ports NewPortType) : Variable(NewPortType) // Created by Board
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

PortClass::~PortClass()
{
    StopDriver();
}