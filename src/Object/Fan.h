class FanClass : public Variable<uint8_t>
{
public:
    enum Module
    {
        Port,
    };

    FanClass(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~FanClass();

    bool Run();
};

FanClass::FanClass(bool New, IDClass ID, FlagClass Flags) : Variable(0, ID, Flags)
{
    BaseClass::Type = Types::Fan;
    Name = "Fan";
    Outputs.Add(this);
};

FanClass::~FanClass()
{
    Outputs.Remove(this);
};

bool FanClass::Run()
{
    PortClass *Port = Modules.Get<PortClass>(Module::Port); // HW connection

    if (Port == nullptr || Data == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    uint8_t* Pin = Port->GetPWM(this);

    if (Pin == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }

    analogWrite(*Pin, *Data);
    return true;
};