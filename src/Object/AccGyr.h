class GyrAccClass : public BaseClass
{
public:
    enum Value
    {
        DeviceType,
        AngularRate,
        Acceleration,
        AngularFilter,
        AccelerationFilter
    };

    bool OK = false;
    TwoWire *I2C = nullptr;

    void Setup(int32_t Index = -1);
    GyrAccClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    bool Run();

    static void SetupBridge(BaseClass *Base, int32_t Index) { static_cast<GyrAccClass *>(Base)->Setup(Index); }
    static bool RunBridge(BaseClass *Base) { return static_cast<GyrAccClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = GyrAccClass::SetupBridge,
        .Run = GyrAccClass::RunBridge,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};
};

constexpr VTable GyrAccClass::Table;

GyrAccClass::GyrAccClass(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags) // Created by Board
{
    Type = ObjectTypes::AccGyr;
    Name = "Acc&Gyr";

    ValueSet(GyrAccs::Undefined);
    ValueSet(Vector3D());
    ValueSet(Vector3D());
    ValueSet(Number(0.5));
    ValueSet(Number(0.5));

    Sensors.Add(this);
};

void GyrAccClass::Setup(int32_t Index)
{
    if (Index != -1 && Index != 0)
        return;

    if (I2C == nullptr)
        return;

    if (Values.Type(DeviceType) != Types::AccGyr)
        return;

    switch (ValueGet<GyrAccs>(DeviceType))
    {
    case GyrAccs::LSM6DS3TRC:
        // Initialization
        I2C->beginTransmission(0b1101010); // 0b1101011 or 0b1101010 + write
        I2C->write(0x10);                  // Subaddress
        I2C->write(0b00110100);            // Acc (+-16G, 52Hz)
        I2C->write(0b00111100);            // Gyro (+-2000dps, 52Hz)
        I2C->endTransmission();
        OK = true;

        // Call first request
        I2C->beginTransmission(0b1101010); // 0b1101011 or 0b1101010 + write
        I2C->write(0x22);                  // Subaddress
        I2C->endTransmission();
        I2C->requestFrom(0b1101010, 12); // 0b1101011 or 0b1101010 + read
        break;
    default:
        break;
    }
}

bool GyrAccClass::Run()
{
    uint16_t Buffer[6];

    if (Values.Type(Acceleration) != Types::Vector3D || Values.Type(AngularRate) != Types::Vector3D || Values.Type(AccelerationFilter) != Types::Number || Values.Type(AngularFilter) != Types::Number)
        return true;

    if (I2C == nullptr)
        return true;

    if (OK == false)
        Setup();

    if (I2C->available() >= 12) // Data ready
    {
        Vector3D AccTemp;
        Vector3D RotTemp;
        Vector3D Acc = ValueGet<Vector3D>(Acceleration);
        Vector3D Rot = ValueGet<Vector3D>(AngularRate);
        Number AccFilter = ValueGet<Number>(AccelerationFilter);
        Number RotFilter = ValueGet<Number>(AngularFilter);

        I2C->readBytes((uint8_t *)Buffer, 12);

        // Request more
        I2C->beginTransmission(0b1101010); // 0b1101011 or 0b1101010 + write
        I2C->write(0x22);                  // Subaddress
        I2C->endTransmission();
        I2C->requestFrom(0b1101010, 12); // 0b1101011 or 0b1101010 + read

        RotTemp.X = (Number(Buffer[0]) / 939) * (1 / (1 + RotFilter)) + Rot.X * (RotFilter / (1 + RotFilter)); // 2000 * (pi / 180) / 32 768
        RotTemp.Y = (Number(Buffer[1]) / 939) * (1 / (1 + RotFilter)) + Rot.Y * (RotFilter / (1 + RotFilter));
        RotTemp.Z = (Number(Buffer[2]) / 939) * (1 / (1 + RotFilter)) + Rot.Z * (RotFilter / (1 + RotFilter));
        AccTemp.X = (Number(Buffer[3]) / 209) * (1 / (1 + AccFilter)) + Acc.X * (AccFilter / (1 + AccFilter)); // 16 * g / 32 768
        AccTemp.Y = (Number(Buffer[4]) / 209) * (1 / (1 + AccFilter)) + Acc.Y * (AccFilter / (1 + AccFilter));
        AccTemp.Z = (Number(Buffer[5]) / 209) * (1 / (1 + AccFilter)) + Acc.Z * (AccFilter / (1 + AccFilter));

        ValueSet(RotTemp, AngularRate);
        ValueSet(AccTemp, Acceleration);
        // Serial.println(Rot->AsString() + " " + Acc->AsString());
    }

    return true;
}