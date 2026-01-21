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
};

InputClass::InputClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    BaseClass::Type = ObjectTypes::Input;
    Name = "Input";

    Values.Add(Inputs::Undefined, InputType);

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

    // Deletion of previous values
    Values.Delete(Input);
    Values.Delete(Indicator);

    switch (*Values.At<Inputs>(InputType))
    {
    case Inputs::ButtonWithLED:
        Values.Add(false, Indicator);
    case Inputs::Button:
        Values.Add(false, Input);
        break;
    case Inputs::Analog:
        Values.Add((int32_t)0, Input);
        break;
    default:
        break;
    }
};

bool InputClass::Run()
{
    Inputs *Type = Values.At<Inputs>(InputType);

    if (Type == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }
    if (Pin == -1)
    {
        ReportError(Status::PortError, "Input");
        return true;
    }

    switch (*Type)
    {
    case Inputs::Button:
        *Values.At<bool>(Input) = !digitalRead(Pin); // Inverted
        break;
    case Inputs::Analog:
        *Values.At<int32_t>(Input) = analogRead(Pin);
        break;
    case Inputs::ButtonWithLED:
        if (*Values.At<bool>(Indicator))
        {
            pinMode(Pin, OUTPUT);
            digitalWrite(Pin, LOW); // Inverted
        }
        else // Applies blocking
        {
            pinMode(Pin, INPUT);
            *Values.At<bool>(Input) = !digitalRead(Pin); // Inverted
        }
        break;
    default:
        break;
    }
    return true;
};