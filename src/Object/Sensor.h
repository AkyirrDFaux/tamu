class SensorClass : public BaseClass
{
public:
    enum Value
    {
        SensorType,
        Measurement,
        Filter
    };
    int8_t Pin = -1;

    SensorClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~SensorClass();

    bool Run();
};

SensorClass::SensorClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
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
    if (Pin == -1)
    {
        ReportError(Status::PortError, "Sensor");
        return true;
    }

    if (Values.Type(SensorType) != Types::Sensor || Values.Type(Measurement) != Types::Number || Values.Type(Filter) != Types::Number)
    {
        ReportError(Status::MissingModule, "Sensor");
        return true;
    }

    Number Filter = ValueGet<Number>(Value::Filter);
    Number In = analogRead(Pin);
    switch (ValueGet<SensorTypes>(SensorType))
    {
    case SensorTypes::AnalogVoltage:
        In = In * VOLTAGE / ADCRES;
    case SensorTypes::TempNTC10K:
        In = 1 / (0.0034 + Number(log(In / (ADCRES - In))) / 3950) - 273.15; //°C
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