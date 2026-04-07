class SensorClass : public BaseClass
{
public:
    Pin MeasPin = INVALID_PIN;
    PortNumber CurrentPort = 255;

    SensorClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});
    ~SensorClass();

    void Setup(uint16_t Index);
    bool Run();

    bool Connect();
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, uint16_t Index)
    {
        static_cast<SensorClass *>(Base)->Setup(Index);
    }
    static bool RunBridge(BaseClass *Base)
    {
        return static_cast<SensorClass *>(Base)->Run();
    }

    static constexpr VTable Table = {
        .Setup = SensorClass::SetupBridge,
        .Run = SensorClass::RunBridge};
};

constexpr VTable SensorClass::Table;

SensorClass::SensorClass(const Reference &ID, FlagClass Flags, RunInfo Info)
    : BaseClass(&SensorClass::Table, ID, Flags, Info)
{
    Type = ObjectTypes::Sensor;
    Name = "Sensor";

    // 1. Root {0}: Sensor Type
    SensorTypes initialType = SensorTypes::Undefined;
    Values.Set(&initialType, sizeof(SensorTypes), Types::Sensor, 0, false, true);

    // 2. Child of Type {0, 0}: Port
    PortNumber initialPort = -1;
    Values.InsertChild(&initialPort, sizeof(PortNumber), Types::Byte, 0, false, true);

    // 3. Sibling of Type {1}: Measurement
    Number zero = 0;
    uint16_t measIdx = Values.InsertNext(&zero, sizeof(Number), Types::Number, 0);

    // 4. Sibling of Measurement {2}: Filter
    Number defaultFilter = 1;
    Values.InsertNext(&defaultFilter, sizeof(Number), Types::Number, measIdx);
}

SensorClass::~SensorClass()
{
    Disconnect();
};

bool SensorClass::Connect()
{
    // Port {0, 0} is the first child of Type {0}
    uint16_t portIdx = Values.Child(0);
    Result res = Values.Get(portIdx);

    if (!res.Value || res.Length < sizeof(PortNumber))
    {
        ReportError(Status::PortError);
        return false;
    }

    PortNumber Port = *(PortNumber*)res.Value;
    if (Port > 10) return false;

    if (Board.ConnectPin(this, Port))
    {
        CurrentPort = Port;
        HW::ModeAnalog(MeasPin);
        return true;
    }

    return false;
}

bool SensorClass::Disconnect()
{
    Board.DisconnectPin(this, CurrentPort);
    CurrentPort = -1;
    return true;
}

void SensorClass::Setup(uint16_t Index)
{
    // Reconnect if the physical port {0, 0} changes
    if (Index == 1)
    {
        Disconnect();
        Connect();
        return;
    }
}

bool SensorClass::Run()
{
    if (!HW::IsValidPin(MeasPin))
    {
        ReportError(Status::PortError);
        return true;
    }

    // Linear navigation: 0 -> Next -> Next
    uint16_t typeIdx = 0;
    uint16_t measIdx = Values.Next(typeIdx);
    uint16_t filtIdx = Values.Next(measIdx);

    Result typeRes  = Values.Get(typeIdx);
    Result measRes  = Values.Get(measIdx);
    Result filtRes  = Values.Get(filtIdx);

    if (!typeRes.Value || !measRes.Value || !filtRes.Value)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    SensorTypes mode = *(SensorTypes*)typeRes.Value;
    Number* measurementPtr = (Number*)measRes.Value;
    Number filterVal = *(Number*)filtRes.Value;

    // Raw Hardware Read
    Number In = HW::AnalogRead(MeasPin);

    // Transformation Logic
    switch (mode)
    {
    case SensorTypes::AnalogVoltage:
        In = In * VOLTAGE / ADCRES;
        break;
        
    case SensorTypes::TempNTC10K:
        // Steinhart-Hart simplified
        In = 1.0f / (0.0034f + log(In / (ADCRES - In)) / 3950.0f) - 273.15f;
        break;
        
    case SensorTypes::Light10K:
        In = sq(18.0f * (ADCRES - In) / In);
        break;
        
    default:
        break;
    }

    // Exponential Moving Average Filter
    // We update the ValueTree memory directly
    Number weightNew = 1.0f / (1.0f + filterVal);
    Number weightOld = 1.0f - weightNew;

    *measurementPtr = (In * weightNew) + (*measurementPtr * weightOld);

    return true;
}