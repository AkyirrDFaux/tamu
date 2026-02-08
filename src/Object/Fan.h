class FanClass : public BaseClass
{
public:
    enum Value
    {
        Speed,
    };
    PWMDriver *PWM = nullptr;

    FanClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~FanClass();

    bool Run();

    static bool RunBridge(BaseClass *Base) { return static_cast<FanClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = FanClass::RunBridge,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};
};

constexpr VTable FanClass::Table;

FanClass::FanClass(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags)
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
        ReportError(Status::MissingModule);
        return true;
    }

    if (PWM == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }

    uint8_t Speed = ValueGet<uint8_t>(Value::Speed);

    double Val = Speed;
    PWM->writeScaled(Val / 255);
    return true;
};