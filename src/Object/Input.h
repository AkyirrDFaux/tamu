class InputClass : public BaseClass
{
public:
    Pin InputPin = INVALID_PIN;
    PortNumber CurrentPort = -1;

    InputClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});
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

InputClass::InputClass(const Reference &ID, FlagClass Flags, RunInfo Info) : BaseClass(&Table, ID, Flags, Info)
{
    Type = ObjectTypes::Input;
    Name = "Input";

    // Initialize using compact path arrays
    Inputs initialMode = Inputs::Undefined;
    Values.Set(&initialMode, sizeof(Inputs), Types::Input, Reference({0}));

    PortNumber initialPort = -1;
    Values.Set(&initialPort, sizeof(PortNumber), Types::PortNumber, Reference({0, 0}));

    bool initialState = false;
    Values.Set(&initialState, sizeof(bool), Types::Bool, Reference({1}));
};

InputClass::~InputClass()
{
    Disconnect();
};

bool InputClass::Connect()
{
    // Search directly in the local ValueTree using Bookmark
    Bookmark portMark = Values.Find({0, 0}, true);
    Result res = Values.Get(portMark);

    if (res.Length < sizeof(PortNumber) || !res.Value)
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

void InputClass::Setup(const Reference &Index)
{
    // Check if the hardware port specifically changed
    if (Index == Reference({0, 0}))
    {
        Disconnect();
        Connect();
        return;
    }

    // Only proceed if the base mode changed
    if (Index != Reference({0}))
        return;

    Bookmark modeMark = Values.Find({0}, true);
    Result res = Values.Get(modeMark);

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
        Values.Set(&resetVal, sizeof(bool), Types::Bool, Reference({1}));
        break;
    default:
        break;
    }
};

bool InputClass::Run()
{
    // Use Bookmarks for efficiency in the hot-loop
    Bookmark modeMark = Values.Find({0}, true);
    Bookmark valMark = Values.Find({1}, true);

    Result modeRes = Values.Get(modeMark);
    Result valRes = Values.Get(valMark);

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
        // Reference {2} is the Indicator State
        Bookmark ledMark = Values.Find({2}, true);
        Result ledRes = Values.Get(ledMark);
        bool ledOn = (ledRes.Value) ? *(bool *)ledRes.Value : false;

        if (ledOn)
        {
            HW::ModeOutput(InputPin);
            HW::Low(InputPin);
        }
        else
        {
            HW::ModeInput(InputPin);
            // Pulse read: Input is active low
            *statePtr = (HW::Read(InputPin) == false);
        }
        break;
    }
    default:
        break;
    }

    return true;
};