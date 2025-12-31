// This might want a custom solution, to properly use the I2C drivers
class GyrAccClass : public BaseClass
{
public:
    enum Value
    {
        DeviceType,
        AngularRate,
        Acceleration,
    };

    bool OK = false;

    void Setup();
    GyrAccClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    bool Run();
};
GyrAccClass::GyrAccClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags) // Created by Board
{
    Type = ObjectTypes::AccGyr;
    Name = "Acc&Gyr";

    Values.Add(GyrAccs::Undefined);
    Values.Add(Vector3D());
    Values.Add(Vector3D());

    Sensors.Add(this);
};

void GyrAccClass::Setup()
{
    if (!(Values.IsValid(DeviceType, Types::AccGyr) && Modules.IsValid(0) && Modules.IsValid(1) && Modules[0]->Type == ObjectTypes::Port && Modules[1]->Type == ObjectTypes::Port))
        return;

    // Delete previous driver, if there was any

    TwoWire *I2C = Modules.Get<PortClass>(0)->GetI2C(this);

    if (I2C == nullptr)
        return;

    switch (*Values.At<GyrAccs>(DeviceType))
    {
    case GyrAccs::LSM6DS3TRC:
        // Initialization
        Wire.beginTransmission(0b1101010); // 0b1101011 or 0b1101010 + write
        Wire.write(0x10);                  // Subaddress
        Wire.write(0b00110100);            // Acc (+-16G, 52Hz)
        Wire.write(0b00111100);            // Gyro (+-2000dps, 52Hz)
        Wire.endTransmission();
        OK = true;

        // Call first request
        Wire.beginTransmission(0b1101010); // 0b1101011 or 0b1101010 + write
        Wire.write(0x22);                  // Subaddress
        Wire.endTransmission();
        Wire.requestFrom(0b1101010, 12); // 0b1101011 or 0b1101010 + read
        break;
    default:
        break;
    }
}

bool GyrAccClass::Run()
{
    uint8_t Buffer[12];
    Vector3D *Acc = Values.At<Vector3D>(Acceleration);
    Vector3D *Rot = Values.At<Vector3D>(AngularRate);

    if (OK == false)
        Setup();

    if (Wire.available() >= 12) // Data ready
    {
        Wire.readBytes(Buffer, 12);

        // Request more
        Wire.beginTransmission(0b1101010); // 0b1101011 or 0b1101010 + write
        Wire.write(0x22);                  // Subaddress
        Wire.endTransmission();
        Wire.requestFrom(0b1101010, 12); // 0b1101011 or 0b1101010 + read

        int16_t num;
        memcpy(&num, Buffer, 2);
        Rot->X = Number(num) / 939; // 2000 * (pi / 180) / 32 768  
        memcpy(&num, Buffer + 2, 2);
        Rot->Y = Number(num) / 939;
        memcpy(&num, Buffer + 4, 2);
        Rot->Z = Number(num) / 939;
        memcpy(&num, Buffer + 6, 2);
        Acc->X = Number(num) / 209; // 16 * g / 32 768 
        memcpy(&num, Buffer + 8, 2);
        Acc->Y = Number(num) / 209;
        memcpy(&num, Buffer + 10, 2);
        Acc->Z = Number(num) / 209;

        //Serial.println(Rot->AsString() + " " + Acc->AsString());
    }

    //*Values.At<Vector3D>(Acceleration) = Vector3D(a.acceleration.x, a.acceleration.y, a.acceleration.z);
    //*Values.At<Vector3D>(AngularRate) = Vector3D(g.gyro.x, g.gyro.y, g.gyro.z);

    return true;
}