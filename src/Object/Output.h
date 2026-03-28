class OutputClass : public BaseClass
{
public:
    Pin PWMPin = INVALID_PIN;
    PortNumber CurrentPort = -1;

    OutputClass(const Reference &ID, ObjectInfo Info = {Flags::None, 1});
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

OutputClass::OutputClass(const Reference &ID, ObjectInfo Info) : BaseClass(&Table, ID, Info)
{
    Type = ObjectTypes::Output;
    Name = "Output";

    // Structure:
    // {0}   : Mode (PWM / Servo)
    // {0,0} : Port Index
    // {1}   : Target Value (Speed/Angle)
    // {1,0} : Frequency (Hz)

    Values.Set(Outputs::Undefined, {0});
    Values.Set<PortNumber>(-1, {0, 0});
    Values.Set((Number)0, {1});
    Values.Set((int32_t)25000, {1, 0}); // Default 25kHz
};

OutputClass::~OutputClass()
{
    Disconnect();
};

bool OutputClass::Connect()
{
    Getter<PortNumber> Port = Values.Get<PortNumber>({0, 0});

    if (!Port.Success || Port.Value > 10)
    {
        ReportError(Status::PortError);
        return false;
    }

    if (Board.ConnectPin(this, Port))
    {
        CurrentPort = Port.Value;

        // Immediate hardware mode setup upon connection
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

    // Update Hardware Mode if enum {0} or frequency {1,0} changes
    if (Index == Reference({0}) || Index == Reference({0, 0}) || Index == Reference({1, 0}))
    {
        if (!HW::IsValidPin(PWMPin))
            return;

        Outputs Mode = Values.Get<Outputs>({0}).Value;
        int32_t Freq = Values.Get<int32_t>({1, 0}).Value;

        if (Mode == Outputs::PWM)
        {
            HW::ModePWM(PWMPin, Freq);
        }
        /*else if (Mode == Outputs::Servo)
        {
            // Servos are almost always 50Hz, usually ignored but can be set here
            HW::ModeServo(PWMPin, Freq);
        }*/
    }
}

bool OutputClass::Run()
{
    if (!HW::IsValidPin(PWMPin))
        return true;

    Getter<Number> Target = Values.Get<Number>({1});
    Getter<Outputs> Mode = Values.Get<Outputs>({0});

    if (!Target.Success || !Mode.Success)
        return true;

    if (Mode.Value == Outputs::PWM)
    {
        HW::PWM(PWMPin, Target.Value);
    }
    /*else if (Mode.Value == Outputs::Servo)
    {
        HW::Servo(PWMPin, Target.Value);
    }*/

    return true;
};