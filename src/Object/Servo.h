class ServoClass : public BaseClass
{
public:
    enum Value
    {
        Angle,
    };
    Servo *ServoDriver;

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
    uint8_t *Angle = Values.At<uint8_t>(Value::Angle);

    if (Angle == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    if (ServoDriver == nullptr)
    {
        ReportError(Status::PortError, "Servo");
        return true;
    }
    ServoDriver->write(*Angle);
    return true;
};