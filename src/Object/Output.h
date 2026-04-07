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

    // 1. Root {0}: Mode
    Outputs initialMode = Outputs::Undefined;
    Values.Set(&initialMode, sizeof(Outputs), Types::Output, 0, false, true);

    // 2. Child of Mode {0, 0}: Port
    PortNumber initialPort = -1;
    Values.InsertChild(&initialPort, sizeof(PortNumber), Types::PortNumber, 0, false, true);

    // 3. Sibling of Mode {1}: Target (Value)
    Number initialTarget = 0;
    uint16_t targetIdx = Values.InsertNext(&initialTarget, sizeof(Number), Types::Number, 0);

    // 4. Child of Target {1, 0}: Frequency
    int32_t initialFreq = 25000;
    Values.InsertChild(&initialFreq, sizeof(int32_t), Types::Integer, targetIdx, false, true);
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
    // Port change {0, 0}
    if (Index == 1)
    {
        Disconnect();
        Connect();
        return;
    }

    // Mode {0} or Frequency {1, 0} change
    bool isMode = (Index == 0);
    bool isFreq = (Index == 3);

    if (isMode || isFreq)
    {
        if (!HW::IsValidPin(PWMPin))
            return;

        uint16_t targetIdx = Values.Next(0);

        Result modeRes = Values.Get(0);
        Result freqRes = Values.Get(Values.Child(targetIdx)); // Freq is child of Target

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

    // Linear navigation: 0 (Mode) -> Next (Target)
    uint16_t modeIdx = 0;
    uint16_t targetIdx = Values.Next(modeIdx);

    Result modeRes = Values.Get(modeIdx);
    Result targetRes = Values.GetThis(targetIdx); // GetThis in case Target is linked

    if (!modeRes.Value || !targetRes.Value)
        return true;

    Outputs Mode = *(Outputs *)modeRes.Value;
    Number Target = *(Number *)targetRes.Value;

    if (Mode == Outputs::PWM)
    {
        HW::PWM(PWMPin, Target);
    }

    return true;
}