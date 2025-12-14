class ServoClass : public BaseClass
{
public:
    enum Value
    {
        Angle,
    };
    enum Module
    {
        Port,
    };

    ServoClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~ServoClass();

    bool Run();
};

ServoClass::ServoClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    BaseClass::Type = ObjectTypes::Servo;
    Values.Add<uint8_t>(0);
    Name = "Servo";
    Outputs.Add(this);
};

ServoClass::~ServoClass()
{
    Outputs.Remove(this);
};

bool ServoClass::Run()
{
    PortClass *Port = Modules.Get<PortClass>(Module::Port); // HW connection
    uint8_t *Angle = Values.At<uint8_t>(Value::Angle);

    if (Port == nullptr || Angle == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    Servo* Driver = Port->GetServo(this);

    if (Driver == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }
    Driver->write(*Angle);
    return true;
};