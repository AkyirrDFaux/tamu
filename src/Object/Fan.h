class FanClass : public Variable<uint8_t>
{
public:
    enum Module
    {
        PortAttach,
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

    if (New)
        AddModule(new PortAttachClass(New), PortAttach);
};

FanClass::~FanClass()
{
    Outputs.Remove(this);
};

bool FanClass::Run()
{
    PortAttachClass *PortAttach = Modules.Get<PortAttachClass>(Module::PortAttach); // HW connection

    if (PortAttach == nullptr || Data == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }
    if (PortAttach->Check(Drivers::FanPWM) == false)
    {
        ReportError(Status::PortError);
        return true;
    }

    analogWrite(PortAttach->Port->Pin, *Data);

    return true;
};