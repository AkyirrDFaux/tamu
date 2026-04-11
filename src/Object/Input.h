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

    if (Port > 10)
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
    // Index 1: Port {0, 0}
    if (Index == 1)
    {
        Disconnect();
        Connect();
        return;
    }

    // Index 0: Mode {0}
    if (Index != 0)
        return;

    Result res = Values.Get(0);
    if (!res.Value) return;

    Inputs mode = *(Inputs *)res.Value;
    bool resetVal = false;

    switch (mode)
    {
    case Inputs::ButtonWithLED:
        // Ensure Index 3 exists. Set() handles the insertion if missing.
        // This is Branch {2}.
        Values.Set(&resetVal, sizeof(bool), Types::Bool, 3, 0); 
        [[fallthrough]];

    case Inputs::Button:
        // Reset Primary Value at Index 2 (Branch {1})
        Values.SetExisting(&resetVal, sizeof(bool), Types::Bool, 2);
        
        // Pruning logic: If we are in standard Button mode, 
        // but a node exists after Index 2, kill it.
        if (mode == Inputs::Button)
        {
            uint16_t extra = Values.Next(2);
            if (extra != INVALID_HEADER)
                Values.Delete(extra);
        }
        break;

    default:
        // Clean up everything after the core hardware config/value
        uint16_t extra = Values.Next(2);
        if (extra != INVALID_HEADER)
            Values.Delete(extra);
        break;
    }

    Disconnect();
    Connect();
}

bool InputClass::Run()
{
    // Hard-indexed mapping (O(1) access):
    // 0: Mode {0}
    // 1: Port {0, 0} (Ignored in Run, used in Setup)
    // 2: Value {1}
    // 3: LED State {2} (Only for ButtonWithLED)

    Result modeRes = Values.Get(0);
    Result valRes  = Values.Get(2);

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

    Inputs mode    = *(Inputs *)modeRes.Value;
    bool *statePtr = (bool *)valRes.Value;

    switch (mode)
    {
    case Inputs::Button:
        // Direct read, zero-copy update to ValueTree Index 2
        *statePtr = (HW::Read(InputPin) == false);
        break;

    case Inputs::ButtonWithLED:
    {
        // Direct access to LED state at Index 3
        Result ledRes = Values.Get(3);
        bool ledOn = (ledRes.Value) ? *(bool *)ledRes.Value : false;

        if (ledOn)
        {
            // Pin Multiplexing: Drive the LED low
            HW::ModeOutput(InputPin);
            HW::Low(InputPin);
        }
        else
        {
            // Pin Multiplexing: Release to Input and read the button
            HW::ModeInput(InputPin);
            *statePtr = (HW::Read(InputPin) == false);
        }
        break;
    }
    default:
        break;
    }

    return true;
}