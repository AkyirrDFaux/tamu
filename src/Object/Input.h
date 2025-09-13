class InputClass : public Variable<Inputs>
{
public:
    enum Module
    {
        Port,
    };

    InputClass(Inputs InputType, bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~InputClass();
    void Setup();

    bool Run();
};

InputClass::InputClass(Inputs InputType, bool New, IDClass ID, FlagClass Flags) : Variable(InputType, ID, Flags)
{
    BaseClass::Type = Types::Input;
    Name = "Input";
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
    switch (*Data)
    {
    case Inputs::ButtonWithLED:
        AddModule(new Variable<bool>(false), 2);
        Modules[2]->Name = "Indicator Switch";
    case Inputs::Button:
        AddModule(new Variable<bool>(false), 1);
        Modules[1]->Name = "Input value";
        break;
    case Inputs::Analog:
        AddModule(new Variable<int32_t>(0), 1);
        Modules[1]->Name = "Input value";
        break;
    default:
        break;
    }
};

bool InputClass::Run()
{
    PortClass *Port = Modules.Get<PortClass>(Module::Port); // HW connection

    if (Port == nullptr || Data == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    uint8_t *Pin = Port->GetInput(this);

    if (Pin == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }

    switch (*Data)
    {
    case Inputs::Button:
        *Modules.GetValue<bool>(1) = !digitalRead(*Pin); //Inverted
        break;
    case Inputs::Analog:
        *Modules.GetValue<int32_t>(1) = analogRead(*Pin);
        break;
    case Inputs::ButtonWithLED:
        if (*Modules.GetValue<bool>(2))
        {
            pinMode(*Pin, OUTPUT);
            digitalWrite(*Pin, LOW); //Inverted
        }
        else //Applies blocking
        {
            pinMode(*Pin, INPUT);
            *Modules.GetValue<bool>(1) = !digitalRead(*Pin); //Inverted
        }
        break;
    default:
        break;
    }
    return true;
};