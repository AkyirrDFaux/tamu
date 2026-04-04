class SensorClass : public BaseClass
{
public:
    Pin MeasPin = INVALID_PIN;
    PortNumber CurrentPort = 255;

    SensorClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1,0});
    ~SensorClass();

    void Setup(const Reference &Index);
    bool Run();

    bool Connect();
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, const Reference &Index)
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

SensorClass::SensorClass(const Reference &ID, FlagClass Flags, RunInfo Info) : BaseClass(&SensorClass::Table, ID, Flags, Info)
{
    Type = ObjectTypes::Sensor;
    Name = "Sensor";

    // Initialize using direct local Set calls
    SensorTypes initialType = SensorTypes::Undefined;
    Values.Set(&initialType, sizeof(SensorTypes), Types::Sensor, Reference({0}));

    PortNumber initialPort = -1;
    Values.Set(&initialPort, sizeof(PortNumber), Types::Byte, Reference({0, 0}));

    Number zero = 0;
    Values.Set(&zero, sizeof(Number), Types::Number, Reference({1})); // Measurement

    Number defaultFilter = 1;
    Values.Set(&defaultFilter, sizeof(Number), Types::Number, Reference({2})); // Filter
};

SensorClass::~SensorClass()
{
    Disconnect();
};

bool SensorClass::Connect()
{
    // Direct local Find for port resolution using Bookmark
    Bookmark portMark = Values.Find({0, 0}, true);
    Result res = Values.Get(portMark);

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
        // Ensure hardware is set to Analog mode
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

void SensorClass::Setup(const Reference &Index)
{
    // Reconnect if the physical port {0, 0} changes
    if (Index == Reference({0, 0}))
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

    // 1. Fetch Bookmarks for core parameters
    Bookmark typeMark   = Values.Find({0}, true);
    Bookmark measMark   = Values.Find({1}, true);
    Bookmark filterMark = Values.Find({2}, true);

    // 2. Resolve Results
    Result typeRes   = Values.Get(typeMark);
    Result measRes   = Values.Get(measMark);
    Result filterRes = Values.Get(filterMark);

    if (!typeRes.Value || !measRes.Value || !filterRes.Value)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    // Map internal memory to local pointers for zero-copy math
    SensorTypes mode = *(SensorTypes*)typeRes.Value;
    Number* measurementPtr = (Number*)measRes.Value;
    Number filterVal = *(Number*)filterRes.Value;

    // 3. Raw Hardware Read
    Number In = HW::AnalogRead(MeasPin);

    // 4. Transformation Logic
    switch (mode)
    {
    case SensorTypes::AnalogVoltage:
        In = In * VOLTAGE / ADCRES;
        break;
        
    case SensorTypes::TempNTC10K:
        // Steinhart-Hart simplified: 1/T = 1/T0 + 1/B * ln(R/R0)
        // Note: Using ADCRES for the voltage divider ratio
        In = 1.0f / (0.0034f + log(In / (ADCRES - In)) / 3950.0f) - 273.15f;
        break;
        
    case SensorTypes::Light10K:
        // Power curve approximation for LDR
        In = sq(18.0f * (ADCRES - In) / In);
        break;
        
    default:
        break;
    }

    // 5. Exponential Moving Average Filter
    // Efficient weight distribution to avoid multiple divisions
    Number invFilterTotal = 1.0f / (1.0f + filterVal);
    Number weightNew      = invFilterTotal;
    Number weightOld      = filterVal * invFilterTotal;

    // Apply Filter directly to ValueTree memory
    *measurementPtr = (In * weightNew) + (*measurementPtr * weightOld);

    return true;
}