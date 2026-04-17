class InputClass : public BaseClass
{
public:
    Pin InputPin = INVALID_PIN;
    PortNumber CurrentPort = -1;

    InputClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});
    ~InputClass();

    void Setup(uint16_t Index);
    bool Run();

    // Port Management
    bool Connect();
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, uint16_t Index)
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

InputClass::InputClass(const Reference &ID, FlagClass Flags, RunInfo Info)
    : BaseClass(&Table, ID, Flags, Info)
{
    Type = ObjectTypes::Input;
    Name = "Input";

    uint16_t cursor = 0;
    Tri ReadOnly = (this->Flags == Flags::Auto) ? Tri::Set : Tri::Reset;

    // --- Branch {0}: Configuration (Depth 0) ---
    Inputs initialMode = Inputs::Undefined;
    Values.Set(&initialMode, sizeof(Inputs), Types::Input, cursor++, 0, ReadOnly, Tri::Set); // Index 0: {0}

    // --- Branch {0} Children (Depth 1) ---
    PortNumber initialPort = -1;
    Values.Set(&initialPort, sizeof(PortNumber), Types::PortNumber, cursor++, 1, ReadOnly, Tri::Set); // Index 1: {0, 0}

    // --- Branch {1}: State (Depth 0 - Sibling of {0}) ---
    // Note: Marked ReadOnly=Set because this is driven by hardware, not user writes.
    bool initialState = false;
    Values.Set(&initialState, sizeof(bool), Types::Bool, cursor++, 0, Tri::Set, Tri::Reset); // Index 2: {1}
}

InputClass::~InputClass()
{
    Disconnect();
};

bool InputClass::Connect()
{
    // Mode is 0. Port {0, 0} is the first child of Mode.
    uint16_t portIdx = Values.Child(0);
    Result res = Values.Get(portIdx); // GetThis in case Port is referenced

    if (!res.Value || res.Length < sizeof(PortNumber))
    {
        ReportError(Status::PortError);
        return false;
    }

    PortNumber Port = *(PortNumber *)res.Value;

    if (Port > PORT_COUNT)
    {
        ReportError(Status::PortError);
        return false;
    }

    if (Board.ConnectPin(this, Port))
    {
        CurrentPort = Port;
        return true;
    }

    return false;
}

bool InputClass::Disconnect()
{
    Board.DisconnectPin(this, CurrentPort);
    CurrentPort = -1;
    return true;
}

void InputClass::Setup(uint16_t Index)
{
    if (Index > 1)
        return;

    if (Index == 0)
    {
        Result res = Values.Get(0);
        if (!res.Value)
            return;

        Inputs mode = *(Inputs *)res.Value;
        bool resetVal = false;

        switch (mode)
        {
        case Inputs::ButtonWithLED:
        case Inputs::ButtonWithLEDInverted:
            // Node {2} at Index 3 for LED control
            Values.Set(&resetVal, sizeof(bool), Types::Bool, 3, 0);
            [[fallthrough]];

        case Inputs::Button:
        case Inputs::ButtonInverted:
            // Node {1} at Index 2 for Primary State
            Values.SetExisting(&resetVal, sizeof(bool), Types::Bool, 2);

            // Clean up if moving from LED mode back to standard Button
            if (mode == Inputs::Button || mode == Inputs::ButtonInverted)
            {
                uint16_t extra = Values.Next(2);
                if (extra != INVALID_HEADER)
                    Values.Delete(extra);
            }
            break;

        default:
            // Cleanup everything after the core value
            uint16_t extra = Values.Next(2);
            if (extra != INVALID_HEADER)
                Values.Delete(extra);
            break;
        }
    }

    Disconnect();
    Connect();
}

bool InputClass::Run()
{
    Result modeRes = Values.Get(0);
    Result valRes = Values.Get(2);

    if (modeRes.Type != Types::Input || !valRes.Value)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    if (!HW::IsValidPin(InputPin))
    {
        ReportError(Status::PortError);
        return true;
    }

    Inputs mode = *(Inputs *)modeRes.Value;
    bool *statePtr = (bool *)valRes.Value;
    
    // Determine inversion status
    bool inverted = (mode == Inputs::ButtonInverted || mode == Inputs::ButtonWithLEDInverted);

    switch (mode)
    {
    case Inputs::Button:
    case Inputs::ButtonInverted:
        // Normal logic: High == true. Inverted logic: High == false.
        *statePtr = inverted ? (HW::Read(InputPin) == false) : (HW::Read(InputPin) == true);
        break;

    case Inputs::ButtonWithLED:
    case Inputs::ButtonWithLEDInverted:
    {
        Result ledRes = Values.Get(3);
        bool ledOn = (ledRes.Value) ? *(bool *)ledRes.Value : false;

        if (ledOn)
        {
            // Drive LED (Active Low assumed for most shared LED/Buttons)
            HW::ModeOutput(InputPin);
            inverted ? HW::Low(InputPin) : HW::High(InputPin);
        }
        else
        {
            HW::ModeInput(InputPin);
            // Read button with appropriate logic
            *statePtr = inverted ? (HW::Read(InputPin) == false) : (HW::Read(InputPin) == true);
        }
        break;
    }
    default:
        break;
    }

    return true;
}