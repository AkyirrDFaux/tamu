class InputClass : public BaseClass
{
public:

    enum Value{
        InputType,
        Input,
        Indicator
    };
    enum Module
    {
        Port,
    };

    InputClass(Inputs InputType, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~InputClass();
    void Setup();

    bool Run();
};

InputClass::InputClass(Inputs InputType, IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    BaseClass::Type = Types::Input;
    Name = "Input";

    Values.Add(InputType);

    Sensors.Add(this);
    Setup();
};

InputClass::~InputClass()
{
    Sensors.Remove(this);
};

void InputClass::Setup()
{
    // Deletion/Replacement of previous modules is missing
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
    PortClass *Port = Modules.Get<PortClass>(Module::Port); // HW connection
    Inputs *Type = Values.At<Inputs>(InputType);

    if (Port == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }

    uint8_t *Pin = Port->GetInput(this);

    if (Pin == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }

    switch (*Type)
    {
    case Inputs::Button:
        *Values.At<bool>(Input) = !digitalRead(*Pin); //Inverted
        break;
    case Inputs::Analog:
        *Values.At<int32_t>(Input) = analogRead(*Pin);
        break;
    case Inputs::ButtonWithLED:
        if (*Values.At<bool>(Indicator))
        {
            pinMode(*Pin, OUTPUT);
            digitalWrite(*Pin, LOW); //Inverted
        }
        else //Applies blocking
        {
            pinMode(*Pin, INPUT);
            *Values.At<bool>(Input) = !digitalRead(*Pin); //Inverted
        }
        break;
    default:
        break;
    }
    return true;
};