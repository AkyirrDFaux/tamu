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
    // Direct local Find for port resolution
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
    if (Index == Reference({0, 0}))
    {
        Disconnect();
        Connect();
        return;
    }
    // Type change logic can be added here if needed
}

bool SensorClass::Run()
{
    if (!HW::IsValidPin(MeasPin))
    {
        ReportError(Status::PortError);
        return true;
    }

    // Direct local Find calls for performance
    SearchResult typeRes   = Values.Find(Reference({0}));
    SearchResult measRes   = Values.Find(Reference({1}));
    SearchResult filterRes = Values.Find(Reference({2}));

    if (!typeRes.Value || !measRes.Value || !filterRes.Value)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    // Map internal memory to local pointers for the math block
    SensorTypes mode = *(SensorTypes*)typeRes.Value;
    Number* measurementPtr = (Number*)measRes.Value;
    Number filterVal = *(Number*)filterRes.Value;

    // 1. Raw Read
    Number In = HW::AnalogRead(MeasPin);

    // 2. Transformation based on SensorType
    switch (mode)
    {
    case SensorTypes::AnalogVoltage:
        In = In * VOLTAGE / ADCRES;
        break;
    case SensorTypes::TempNTC10K:
        // Steinhart-Hart simplified
        In = 1 / (0.0034 + log(In / (ADCRES - In)) / 3950) - 273.15;
        break;
    case SensorTypes::Light10K:
        In = sq(18 * (ADCRES - In) / In);
        break;
    default:
        break;
    }

    // 3. Filter calculation and direct memory update
    // Formula: In * (1 / (1 + Filter)) + LastValue * (Filter / (1 + Filter))
    Number invFilter = 1.0f / (1.0f + filterVal);
    *measurementPtr = (In * invFilter) + (*measurementPtr * (filterVal * invFilter));

    return true;
};