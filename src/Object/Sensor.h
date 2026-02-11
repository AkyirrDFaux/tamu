class SensorClass : public BaseClass
{
public:
    enum Value
    {
        SensorType,
        Measurement,
        Filter
    };
    Pin MeasPin = INVALID_PIN;

    SensorClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~SensorClass();

    bool Run();

    static bool RunBridge(BaseClass *Base) { return static_cast<SensorClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = SensorClass::RunBridge,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};
};

constexpr VTable SensorClass::Table;

SensorClass::SensorClass(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags)
{
    BaseClass::Type = ObjectTypes::Sensor;
    Name = "Sensor";

    ValueSet(SensorTypes::Undefined, SensorType);
    ValueSet<Number>(-1, Measurement);
    ValueSet<Number>(1, Filter);
    Sensors.Add(this);
};

SensorClass::~SensorClass()
{
    Sensors.Remove(this);
};

bool SensorClass::Run()
{
    if (HW::IsValidPin(MeasPin) == false)
    {
        ReportError(Status::PortError, ID);
        return true;
    }

    if (Values.Type(SensorType) != Types::Sensor || Values.Type(Measurement) != Types::Number || Values.Type(Filter) != Types::Number)
    {
        ReportError(Status::MissingModule, ID);
        return true;
    }

    Number Filter = ValueGet<Number>(Value::Filter);
    Number In = HW::AnalogRead(MeasPin);
    switch (ValueGet<SensorTypes>(SensorType))
    {
    case SensorTypes::AnalogVoltage:
        In = In * VOLTAGE / ADCRES;
    case SensorTypes::TempNTC10K:
        In = 1 / (0.0034 + log(In / (ADCRES - In)) / 3950) - 273.15; // °C
        break;
    case SensorTypes::Light10K:
        In = sq(18 * (ADCRES - In) / In); // Lux
        break;
    default:
        break;
    }
    ValueSet<Number>(In * (1 / (1 + Filter)) + ValueGet<Number>(Value::Measurement) * (Filter / (1 + Filter)), Measurement);
    return true;
};