class InputClass : public BaseClass
{
public:
    Pin InputPin = INVALID_PIN;
    PortNumber CurrentPort = -1;

    InputClass(const Reference &ID, ObjectInfo Info = {Flags::None, 1});
    ~InputClass();

    void Setup(const Reference &Index);
    bool Run();

    // Port Management
    bool Connect();
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, const Reference &Index)
    {
        static_cast<InputClass *>(Base)->Setup(Index);
    }
    static bool RunBridge(BaseClass *Base)
    {
        return static_cast<InputClass *>(Base)->Run();
    }

    static constexpr VTable Table = {
        .Setup = InputClass::SetupBridge,
        .Run = InputClass::RunBridge};
};

constexpr VTable InputClass::Table;

InputClass::InputClass(const Reference &ID, ObjectInfo Info) : BaseClass(&Table, ID, Info)
{
    Type = ObjectTypes::Input;
    Name = "Input";

    // Initialize the structure
    Values.Set(Inputs::Undefined, {0}); // {0}   : Input Mode/Type
    Values.Set<PortNumber>(-1, {0, 0});   // {0,0} : Port Index (Physical Pin)
    Values.Set(false, {1});             // {1}   : Read Value
    //Values.Set(false, {2});             // {2}   : Indicator State
};

InputClass::~InputClass()
{
    Disconnect();
};

bool InputClass::Connect()
{
    // 1. Retrieve Port Index from {0, 0}
    Getter<PortNumber> Port = Values.Get<PortNumber>({0, 0});

    // 2. Validate (Assuming 0-10 are valid physical ports on your board)
    if (!Port.Success || Port.Value > 10)
    {
        ReportError(Status::PortError);
        return false;
    }

    // 3. Ask the Board to link this object to the physical pin
    // Board handles pin-sharing checks and hardware Registry lookups
    if (Board.Connect(this, Port))
    {
        CurrentPort = Port;
        return true;
    }

    return false;
}

bool InputClass::Disconnect()
{
    Board.Disconnect(this, CurrentPort);
    CurrentPort = -1;
    return true;
}

void InputClass::Setup(const Reference &Index)
{
    // If the index refers to the port {0,0}, attempt reconnection
    if (Index == Reference({0, 0}))
    {
        Disconnect();
        Connect();
        return;
    }

    if (Index != Reference({0}))
        return;

    if (Values.Type({0}) != Types::Input)
        return;

    // Reset values based on mode selection
    switch (Values.Get<Inputs>({0}))
    {
    case Inputs::ButtonWithLED:
        Values.Set(false, {2}); // Reset Indicator
        [[fallthrough]];
    case Inputs::Button:
        Values.Set(false, {1}); // Reset Value
        break;
    default:
        break;
    }
};

bool InputClass::Run()
{
    if (Values.Type({0}) != Types::Input)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    if (HW::IsValidPin(InputPin) == false)
    {
        ReportError(Status::PortError);
        return true;
    }

    switch (Values.Get<Inputs>({0}))
    {
    case Inputs::Button:
        Values.Set<bool>(HW::Read(InputPin) == false, {1}); //Inverted logic, Low = On
        break;

    case Inputs::ButtonWithLED:
        // Multiplexing logic
        if (Values.Get<bool>({2})) // Check Indicator state
        {
            HW::ModeOutput(InputPin);
            HW::Low(InputPin); //Inverted logic, Low = On
        }
        else
        {
            HW::ModeInput(InputPin);
            Values.Set<bool>(HW::Read(InputPin) == false, {1}); //Inverted logic, Low = On
        }
        break;
    default:
        break;
    }

    return true;
};