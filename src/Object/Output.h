class OutputClass : public BaseClass
{
public:
    Pin PWMPin = INVALID_PIN;
    PortNumber CurrentPort = -1;

    OutputClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1,0});
    ~OutputClass();

    void Setup(const Reference &Index);
    bool Run();

    bool Connect();
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, const Reference &Index)
    {
        static_cast<OutputClass *>(Base)->Setup(Index);
    }
    static bool RunBridge(BaseClass *Base)
    {
        return static_cast<OutputClass *>(Base)->Run();
    }

    static constexpr VTable Table = {
        .Setup = OutputClass::SetupBridge,
        .Run = OutputClass::RunBridge};
};

OutputClass::OutputClass(const Reference &ID, FlagClass Flags, RunInfo Info) : BaseClass(&Table, ID, Flags, Info)
{
    Type = ObjectTypes::Output;
    Name = "Output";

    // Initialize the structure using direct ValueTree Set
    Outputs initialMode = Outputs::Undefined;
    Values.Set(&initialMode, sizeof(Outputs), Types::Output, Reference({0}));

    PortNumber initialPort = -1;
    Values.Set(&initialPort, sizeof(PortNumber), Types::Byte, Reference({0, 0}));

    Number initialTarget = 0;
    Values.Set(&initialTarget, sizeof(Number), Types::Number, Reference({1}));

    int32_t initialFreq = 25000;
    Values.Set(&initialFreq, sizeof(int32_t), Types::Integer, Reference({1, 0}));
};

OutputClass::~OutputClass()
{
    Disconnect();
};

bool OutputClass::Connect()
{
    SearchResult res = Values.Find(Reference({0, 0}), true);

    if (res.Length < sizeof(PortNumber) || !res.Value)
    {
        ReportError(Status::PortError);
        return false;
    }

    PortNumber Port = *(PortNumber*)res.Value;

    if (Port > 10)
    {
        ReportError(Status::PortError);
        return false;
    }

    if (Board.ConnectPin(this, Port))
    {
        CurrentPort = Port;
        // Trigger hardware mode setup
        Setup(Reference({0}));
        return true;
    }

    return false;
}

bool OutputClass::Disconnect()
{
    if (CurrentPort.IsValid())
    {
        // Safety: Turn off output before disconnecting
        if (HW::IsValidPin(PWMPin))
            HW::PWM(PWMPin, 0);

        Board.DisconnectPin(this, CurrentPort);
        CurrentPort = -1;
        PWMPin = INVALID_PIN;
    }
    return true;
}

void OutputClass::Setup(const Reference &Index)
{
    // Reconnect on Port change
    if (Index == Reference({0, 0}))
    {
        Disconnect();
        Connect();
        return;
    }

    // Update Hardware Mode if Mode {0} or frequency {1,0} changes
    if (Index == Reference({0}) || Index == Reference({1, 0}))
    {
        if (!HW::IsValidPin(PWMPin))
            return;

        SearchResult modeRes = Values.Find(Reference({0}), true);
        SearchResult freqRes = Values.Find(Reference({1, 0}), true);

        if (modeRes.Value && freqRes.Value)
        {
            Outputs Mode = *(Outputs*)modeRes.Value;
            int32_t Freq = *(int32_t*)freqRes.Value;

            if (Mode == Outputs::PWM)
            {
                HW::ModePWM(PWMPin, Freq);
            }
            // else if (Mode == Outputs::Servo) { ... }
        }
    }
}

bool OutputClass::Run()
{
    if (!HW::IsValidPin(PWMPin))
        return true;

    // Direct search for the target value and mode
    SearchResult targetRes = Values.Find(Reference({1}));
    SearchResult modeRes   = Values.Find(Reference({0}), true);

    if (!targetRes.Value || !modeRes.Value)
        return true;

    Outputs Mode  = *(Outputs*)modeRes.Value;
    Number Target = *(Number*)targetRes.Value;

    if (Mode == Outputs::PWM)
    {
        HW::PWM(PWMPin, Target);
    }
    // else if (Mode == Outputs::Servo) { ... }

    return true;
};