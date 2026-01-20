class FanClass : public BaseClass
{
public:
    enum Value
    {
        Speed,
    };
    ESP32PWM *PWM = nullptr;

    FanClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~FanClass();

    bool Run();
};

FanClass::FanClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    BaseClass::Type = ObjectTypes::Fan;
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
    uint8_t *Speed = Values.At<uint8_t>(Value::Speed);

    if (Speed == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    if (PWM == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }
    double Val = *Speed;
    PWM->writeScaled(Val / 255);
    return true;
};