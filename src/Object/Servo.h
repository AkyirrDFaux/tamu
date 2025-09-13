class ServoClass : public Variable<uint8_t>
{
public:
    enum Module
    {
        Port,
    };

    ServoClass(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~ServoClass();

    bool Run();
};

ServoClass::ServoClass(bool New, IDClass ID, FlagClass Flags) : Variable(0, ID, Flags)
{
    BaseClass::Type = Types::Servo;
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

    if (Port == nullptr || Data == nullptr)
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
    Driver->write(*Data);
    return true;
};