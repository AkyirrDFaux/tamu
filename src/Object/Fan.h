class FanClass : public BaseClass
{
public:
    enum Value
    {
        Speed,
    };

    enum Module
    {
        Port,
    };

    FanClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~FanClass();

    bool Run();
};

FanClass::FanClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    BaseClass::Type = Types::Fan;
    Name = "Fan";

    Values.Add<uint8_t>(0);
    Outputs.Add(this);
};

FanClass::~FanClass()
{
    Outputs.Remove(this);
};

bool FanClass::Run()
{
    PortClass *Port = Modules.Get<PortClass>(Module::Port); // HW connection
    uint8_t *Speed = Values.At<uint8_t>(Value::Speed);

    if (Port == nullptr || Speed == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    ESP32PWM *PWM = Port->GetPWM(this);

    if (PWM == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }
    double Val = *Speed;
    PWM->writeScaled(Val / 255);
    return true;
};