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

    ValueSet<uint8_t>(0);
    Outputs.Add(this);
};

FanClass::~FanClass()
{
    Outputs.Remove(this);
};

bool FanClass::Run()
{
    if (Values.Type(Speed) != Types::Byte)
    {
        ReportError(Status::MissingModule, "Fan");
        return true;
    }

    if (PWM == nullptr)
    {
        ReportError(Status::PortError, "Fan");
        return true;
    }

    uint8_t Speed = ValueGet<uint8_t>(Value::Speed);

    

    double Val = Speed;
    PWM->writeScaled(Val / 255);
    return true;
};