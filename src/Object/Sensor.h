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

    uint16_t cursor = 0;
    Tri ReadOnly = (this->Flags == Flags::Auto) ? Tri::Set : Tri::Reset;

    // Index 0: Sensor Type {0} (Depth 0)
    SensorTypes initialType = SensorTypes::Undefined;
    Values.Set(&initialType, sizeof(SensorTypes), Types::Sensor, cursor++, 0, ReadOnly, Tri::Set);

    // Index 1: Port {0, 0} (Depth 1) - Child of Type
    PortNumber initialPort = -1;
    Values.Set(&initialPort, sizeof(PortNumber), Types::PortNumber, cursor++, 1, ReadOnly, Tri::Set);

    // Index 2: Measurement {1} (Depth 0) - Sibling of Type
    Number zero = N(0.0);
    Values.Set(&zero, sizeof(Number), Types::Number, cursor++, 0, Tri::Set, Tri::Reset);

    // Index 3: Filter {2} (Depth 0) - Sibling of Measurement
    Number defaultFilter = N(1.0);
    Values.Set(&defaultFilter, sizeof(Number), Types::Number, cursor++, 0, Tri::Reset, Tri::Reset);
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

    // Hard-indexed mapping:
    // 0: Type {0}
    // 2: Measurement {1}
    // 3: Filter {2}
    
    Result typeRes = Values.Get(0);
    Result measRes = Values.Get(2);
    Result filtRes = Values.Get(3);

    if (!typeRes.Value || !measRes.Value || !filtRes.Value)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    SensorTypes mode      = *(SensorTypes*)typeRes.Value;
    Number* measurementPtr = (Number*)measRes.Value;
    Number filterVal       = *(Number*)filtRes.Value;

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
        // Note: Number class should handle the log() conversion or casting to float
        In = 1.0 / (0.0034 + log(In / (ADCRES - In)) / 3950.0) - 273.15;
        break;
        
    case SensorTypes::Light10K:
        // Inverse square law approximation for photoresistors
        In = sq(18.0 * (ADCRES - In) / In);
        break;
        
    default:
        break;
    }

    // Exponential Moving Average Filter
    // Using your established weight logic
    Number weightNew = 1.0 / (1.0 + filterVal);
    Number weightOld = 1.0 - weightNew;

    // Zero-copy update directly into DataArray
    *measurementPtr = (In * weightNew) + (*measurementPtr * weightOld);

    return true;
}