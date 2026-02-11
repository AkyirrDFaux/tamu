class InputClass : public BaseClass
{
public:
    enum Value
    {
        InputType,
        Input,
        Indicator
    };
    Pin InputPin = INVALID_PIN;

    InputClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~InputClass();
    void Setup(int32_t Index = -1);
    bool Run();

    static void SetupBridge(BaseClass *Base, int32_t Index) { static_cast<InputClass *>(Base)->Setup(Index); }
    static bool RunBridge(BaseClass *Base) { return static_cast<InputClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = InputClass::SetupBridge,
        .Run = InputClass::RunBridge,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};
};

constexpr VTable InputClass::Table;

InputClass::InputClass(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags)
{
    BaseClass::Type = ObjectTypes::Input;
    Name = "Input";

    ValueSet(Inputs::Undefined, InputType);

    Sensors.Add(this);
};

InputClass::~InputClass()
{
    Sensors.Remove(this);
};

void InputClass::Setup(int32_t Index)
{
    if (Index != -1 && Index != 0)
        return;

    if (Values.Type(InputType) != Types::Input)
        return;

    switch (ValueGet<Inputs>(InputType))
    {
    case Inputs::ButtonWithLED:
        ValueSet(false, Indicator);
    case Inputs::Button:
        ValueSet(false, Input);
        break;
    default:
        break;
    }
};

bool InputClass::Run()
{
    if (Values.Type(InputType) != Types::Input)
    {
        ReportError(Status::MissingModule, ID);
        return true;
    }

    if (HW::IsValidPin(InputPin) == false)
    {
        ReportError(Status::PortError, ID);
        return true;
    }

    switch (ValueGet<Inputs>(InputType))
    {
    case Inputs::Button:
        ValueSet<bool>(HW::Read(InputPin) == true, Input);
        break;
    case Inputs::ButtonWithLED:
        if (ValueGet<bool>(Indicator))
        {
            HW::ModeOutput(InputPin);
            HW::High(InputPin);
        }
        else // Applies blocking
        {
            HW::ModeInput(InputPin);
            ValueSet<bool>(HW::Read(InputPin) == true, Input);
        }
        break;
    default:
        break;
    }
    return true;
};