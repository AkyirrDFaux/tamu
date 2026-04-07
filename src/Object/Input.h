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

    bool ReadOnly = (this->Flags == Flags::Auto);
    // Build the structure linearly:
    // 1. Root {0}: Mode
    Inputs initialMode = Inputs::Undefined;
    Values.Set(&initialMode, sizeof(Inputs), Types::Input, 0, ReadOnly, true);

    // 2. Child of Mode {0, 0}: Port
    PortNumber initialPort = -1;
    Values.InsertChild(&initialPort, sizeof(PortNumber), Types::PortNumber, 0, ReadOnly, true);

    // 3. Sibling of Mode {1}: Value
    bool initialState = false;
    Values.InsertNext(&initialState, sizeof(bool), Types::Bool, 0, true);
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
    // Path {0, 0}: Port changed
    if (Index == 1)
    {
        Disconnect();
        Connect();
        return;
    }

    // Only proceed if the base mode {0} changed
    if (Index != 0)
        return;

    // Mode is at index 0
    Result res = Values.Get(0);
    if (res.Type != Types::Input || !res.Value)
        return;

    Inputs mode = *(Inputs *)res.Value;
    bool resetVal = false;

    switch (mode)
    {
    case Inputs::ButtonWithLED:
        Values.Set(&resetVal, sizeof(bool), Types::Bool, Reference({2}));
        [[fallthrough]];
    case Inputs::Button:
        Values.Set(&resetVal, sizeof(bool), Types::Bool, Reference({1}), true);
        break;
    default:
        break;
    }
}

bool InputClass::Run()
{
    // 1. Root is 0. Mode {0} is the first child of the root.
    uint16_t modeIdx = 0;
    Result modeRes = Values.Get(modeIdx);

    // 2. Value {1} is the sibling immediately following Mode.
    uint16_t valIdx = Values.Next(modeIdx);
    Result valRes = Values.Get(valIdx);

    // Fast validation
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

    switch (mode)
    {
    case Inputs::Button:
        *statePtr = (HW::Read(InputPin) == false);
        break;

    case Inputs::ButtonWithLED:
    {
        // 3. LED State {2} is the sibling immediately following Value.
        uint16_t ledIdx = Values.Next(valIdx);
        Result ledRes = Values.GetThis(ledIdx);
        
        bool ledOn = (ledRes.Value) ? *(bool *)ledRes.Value : false;

        if (ledOn)
        {
            HW::ModeOutput(InputPin);
            HW::Low(InputPin);
        }
        else
        {
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