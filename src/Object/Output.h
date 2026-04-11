class OutputClass : public BaseClass
{
public:
    Pin PWMPin = INVALID_PIN;
    PortNumber CurrentPort = -1;

    OutputClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});
    ~OutputClass();

    void Setup(uint16_t Index);
    bool Run();

    bool Connect();
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, uint16_t Index)
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

OutputClass::OutputClass(const Reference &ID, FlagClass Flags, RunInfo Info)
    : BaseClass(&Table, ID, Flags, Info)
{
    Type = ObjectTypes::Output;
    Name = "Output";

    uint16_t cursor = 0;
    Tri ReadOnly = (this->Flags == Flags::Auto) ? Tri::Set : Tri::Reset;

    // Index 0: Mode {0} (Depth 0)
    Outputs initialMode = Outputs::Undefined;
    Values.Set(&initialMode, sizeof(Outputs), Types::Output, cursor++, 0, ReadOnly, Tri::Set);

    // Index 1: Port {0, 0} (Depth 1)
    PortNumber initialPort = -1;
    Values.Set(&initialPort, sizeof(PortNumber), Types::PortNumber, cursor++, 1, ReadOnly, Tri::Set);

    // Index 2: Target/Value {1} (Depth 0)
    Number initialTarget = 0.0;
    Values.Set(&initialTarget, sizeof(Number), Types::Number, cursor++, 0, Tri::Reset, Tri::Reset);

    // Index 3: Frequency {1, 0} (Depth 1) - Child of Target
    int32_t initialFreq = 25000;
    Values.Set(&initialFreq, sizeof(int32_t), Types::Integer, cursor++, 1, ReadOnly, Tri::Set);
}

OutputClass::~OutputClass()
{
    Disconnect();
};

bool OutputClass::Connect()
{
    // Mode is 0. Port {0, 0} is the first child of Mode.
    uint16_t portIdx = Values.Child(0);
    Result res = Values.Get(portIdx);

    if (!res.Value || res.Length < sizeof(PortNumber))
    {
        ReportError(Status::PortError);
        return false;
    }

    PortNumber Port = *(PortNumber *)res.Value;
    if (Port > 10)
        return false;

    if (Board.ConnectPin(this, Port))
    {
        CurrentPort = Port;
        Setup(0); // Initialize hardware settings
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

void OutputClass::Setup(uint16_t Index)
{
    // Hard-indexed mapping:
    // 0: Mode {0}
    // 1: Port {0, 0}
    // 2: Target {1}
    // 3: Frequency {1, 0}

    // 1. Port changed: Re-link hardware
    if (Index == 1)
    {
        Disconnect();
        Connect();
        return;
    }

    // 2. Mode or Frequency changed
    if (Index == 0 || Index == 3)
    {
        if (!HW::IsValidPin(PWMPin))
            return;

        Result modeRes = Values.Get(0);
        Result freqRes = Values.Get(3);

        if (modeRes.Value && freqRes.Value)
        {
            Outputs Mode = *(Outputs *)modeRes.Value;
            int32_t Freq = *(int32_t *)freqRes.Value;

            if (Mode == Outputs::PWM)
                HW::ModePWM(PWMPin, Freq);
        }
    }
}

bool OutputClass::Run()
{
    if (!HW::IsValidPin(PWMPin))
        return true;

    // Direct jump to Mode and Target (O(1))
    Result modeRes = Values.Get(0);
    Result targetRes = Values.Get(2); // Index 2 is Target {1}

    if (!modeRes.Value || !targetRes.Value)
        return true;

    Outputs Mode = *(Outputs *)modeRes.Value;
    Number Target = *(Number *)targetRes.Value;

    if (Mode == Outputs::PWM)
    {
        // Write duty cycle to timer compare register
        HW::PWM(PWMPin, Target);
    }

    return true;
}