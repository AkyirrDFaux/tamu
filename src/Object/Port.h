class PortClass : public BaseClass
{
public:
    enum Value{
        PortType,
        Pin,
        DriverType
    };

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
    uint8_t *GetInput(BaseClass *ThatObject);
};

PortClass::PortClass(uint8_t NewPin, Ports NewPortType) : BaseClass() // Created by Board
{
    Values.Add(NewPortType);
    Values.Add(NewPin);
    Values.Add(Drivers::None);

    Type = Types::Port;
    Name = "Port";
    Flags = Flags::Auto;
};

PortClass::~PortClass()
{
    StopDriver();
}