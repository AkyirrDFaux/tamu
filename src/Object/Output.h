class FanClass : public BaseClass
{
public:
    enum Value
    {
        Speed,
    };
    Pin PWMPin = INVALID_PIN;

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

    ValueSet<Number>(0, Speed);
    Outputs.Add(this);
};

FanClass::~FanClass()
{
    Outputs.Remove(this);
};

bool FanClass::Run()
{
    if (HW::IsValidPin(PWMPin) == false)
    {
        ReportError(Status::PortError, ID);
        return true;
    }

    if (Values.Type(Speed) != Types::Number)
    {
        ReportError(Status::MissingModule, ID);
        return true;
    }

    HW::PWM(PWMPin, ValueGet<Number>(Speed));
    return true;
};