class InputClass : public BaseClass
{
public:
    enum Value
    {
        InputType,
        Input,
        Indicator
    };
    int8_t Pin = -1;

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
        ReportError(Status::MissingModule, "Input");
        return true;
    }

    if (Pin == -1)
    {
        ReportError(Status::PortError, "Input");
        return true;
    }

    switch (ValueGet<Inputs>(InputType))
    {
    case Inputs::Button:
        ValueSet<bool>(digitalRead(Pin) == ON_STATE, Input);
        break;
    case Inputs::ButtonWithLED:
        if (ValueGet<bool>(Indicator))
        {
            pinMode(Pin, OUTPUT);
            digitalWrite(Pin, ON_STATE);
        }
        else // Applies blocking
        {
            pinMode(Pin, INPUT);
            ValueSet<bool>(digitalRead(Pin) == ON_STATE, Input);
        }
        break;
    default:
        break;
    }
    return true;
};