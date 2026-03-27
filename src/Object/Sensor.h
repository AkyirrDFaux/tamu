class SensorClass : public BaseClass
{
public:
    Pin MeasPin = INVALID_PIN;
    uint8_t CurrentPort = 255;

    SensorClass(const Reference &ID, ObjectInfo Info = {Flags::None, 1});
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

SensorClass::SensorClass(const Reference &ID, ObjectInfo Info) : BaseClass(&Table, ID, Info)
{
    Type = ObjectTypes::Sensor;
    Name = "Sensor";

    // Initialize the structure based on your reference
    Values.Set(SensorTypes::Undefined, {0}); // {0}   : SensorType
    Values.Set<uint8_t>(255, {0, 0});        // {0,0} : Port Index
    Values.Set<Number>(0, {1});             // {1}   : Measurement
    Values.Set<Number>(1, {2});              // {2}   : Filter
};

SensorClass::~SensorClass()
{
    Disconnect();
};

bool SensorClass::Connect()
{
    Getter<uint8_t> Port = Values.Get<uint8_t>({0, 0});

    if (!Port.Success || Port.Value > 10)
    {
        ReportError(Status::PortError);
        return false;
    }

    if (Board.Connect(this, Port.Value))
    {
        CurrentPort = Port.Value;
        HW::ModeAnalog(MeasPin);
        return true;
    }

    return false;
}

bool SensorClass::Disconnect()
{
    Board.Disconnect(this, CurrentPort);
    CurrentPort = 255;
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

    if (Index != Reference({0}))
        return;

    // Reset logic if needed when type changes
};

bool SensorClass::Run()
{
    if (HW::IsValidPin(MeasPin) == false)
    {
        ReportError(Status::PortError);
        return true;
    }

    // Validation of paths {0}, {1}, and {2}
    if (Values.Type({0}) != Types::Sensor || Values.Type({1}) != Types::Number || Values.Type({2}) != Types::Number)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    Number Filter = Values.Get<Number>({2});
    Number In = HW::AnalogRead(MeasPin);

    switch (Values.Get<SensorTypes>({0}))
    {
    case SensorTypes::AnalogVoltage:
        In = In * VOLTAGE / ADCRES;
        break;
    case SensorTypes::TempNTC10K:
        In = 1 / (0.0034 + log(In / (ADCRES - In)) / 3950) - 273.15; // °C
        break;
    case SensorTypes::Light10K:
        In = sq(18 * (ADCRES - In) / In); // Lux
        break;
    default:
        break;
    }

    // Original Filter Math: In * (1 / (1 + Filter)) + LastValue * (Filter / (1 + Filter))
    Number lastMeasurement = Values.Get<Number>({1});
    Number filteredResult = In * (1 / (1 + Filter)) + lastMeasurement * (Filter / (1 + Filter));

    Values.Set<Number>(filteredResult, {1});

    return true;
};