// Temporary
class GyrAccClass : public BaseClass
{
public:
enum Value
    {
        DeviceType,
        AngularRate,
        Acceleration,
    };

    void *Sensor = nullptr;

    void Setup();
    GyrAccClass(GyrAccs NewDevType, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    bool Run();
};
GyrAccClass::GyrAccClass(GyrAccs NewDevType, IDClass ID, FlagClass Flags) : BaseClass(ID, Flags) // Created by Board
{
    Type = Types::AccGyr;
    Name = "Acc&Gyr";
    Flags = Flags::Auto;

    Values.Add(NewDevType);
    //Values.Add(Vector3D());
    //Values.Add(Vector3D());

    Sensors.Add(this);
};

void GyrAccClass::Setup()
{
    if (!(Modules.IsValid(0) && Modules.IsValid(1) && Modules[0]->Type == Types::Port && Modules[1]->Type == Types::Port))
        return;

    TwoWire *I2C = Modules.Get<PortClass>(0)->GetI2C(this); // da 4, cl 5

    if (I2C == nullptr)
        return;

    switch (*Values.At<GyrAccs>(DeviceType))
    {
    case GyrAccs::LSM6DS3TRC:
        Sensor = new Adafruit_LSM6DS3TRC();
        if (!((Adafruit_LSM6DS3TRC *)Sensor)->begin_I2C(0b1101011, I2C)) // 0b1101011 or 0b1101010
            Serial.println("Failed to find LSM6DS3TR-C chip");

        ((Adafruit_LSM6DS3TRC *)Sensor)->setAccelDataRate(LSM6DS_RATE_104_HZ);
        ((Adafruit_LSM6DS3TRC *)Sensor)->setGyroDataRate(LSM6DS_RATE_104_HZ);
        ((Adafruit_LSM6DS3TRC *)Sensor)->setAccelRange(LSM6DS_ACCEL_RANGE_16_G);
        ((Adafruit_LSM6DS3TRC *)Sensor)->setGyroRange(LSM6DS_GYRO_RANGE_2000_DPS);
        break;

    default:
        break;
    }
}

bool GyrAccClass::Run()
{
    sensors_event_t a;
    sensors_event_t g;
    sensors_event_t t;
    if (((Adafruit_LSM6DS3TRC *)Sensor)->getEvent(&a, &g, &t))
    {
        // Compensate for gravity
        //*Values.At<Vector3D>(Acceleration) = Vector3D(a.acceleration.x, a.acceleration.y, a.acceleration.z);
        //*Values.At<Vector3D>(AngularRate) = Vector3D(g.gyro.x, g.gyro.y, g.gyro.z);

        // Add orientation from gravity
    }
    return true;
}