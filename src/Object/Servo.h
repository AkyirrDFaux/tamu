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

    static bool RunBridge(BaseClass *Base) { return static_cast<ServoClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = ServoClass::RunBridge,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};

    bool Run();
};

constexpr VTable ServoClass::Table;

ServoClass::ServoClass(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags)
{
    BaseClass::Type = ObjectTypes::Servo;
    ValueSet<uint8_t>(0);
    Name = "Servo";
    Outputs.Add(this);
};

ServoClass::~ServoClass()
{
    Outputs.Remove(this);
};

bool ServoClass::Run()
{
    if (Values.Type(Angle) != Types::Byte)
    {
        ReportError(Status::MissingModule, "Servo");
        return true;
    }

    if (ServoDriver == nullptr)
    {
        ReportError(Status::PortError, "Servo");
        return true;
    }

    ServoDriver->write(ValueGet<uint8_t>(Value::Angle));
    return true;
};