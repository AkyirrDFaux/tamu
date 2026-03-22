class I2CDeviceClass : public BaseClass
{
public:
    I2C *I2CDriver = nullptr;
    uint8_t CurrentSDA = 255;
    uint8_t CurrentSCL = 255;

    void Setup(Path Index);
    I2CDeviceClass(const Reference &ID, ObjectInfo Info = {Flags::None, 1});
    bool Run();

    bool Connect();
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, Path Index) { static_cast<I2CDeviceClass *>(Base)->Setup(Index); }
    static bool RunBridge(BaseClass *Base) { return static_cast<I2CDeviceClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = I2CDeviceClass::SetupBridge,
        .Run = I2CDeviceClass::RunBridge};
};

I2CDeviceClass::I2CDeviceClass(const Reference &ID, ObjectInfo Info) : BaseClass(&Table, ID, Info)
{
    Type = ObjectTypes::I2C;
    Name = "I2C Device";

    Values.Set(I2CDevices::Undefined, {0});
    Values.Set<uint8_t>(255, {0, 0}); // SDA Port Index
    Values.Set<uint8_t>(255, {0, 1}); // SCL Port Index
    Values.Set<uint8_t>(0, {0, 2});   // I2C Address
};

bool I2CDeviceClass::Connect()
{
    Getter<uint8_t> SDA = Values.Get<uint8_t>({0, 0});
    Getter<uint8_t> SCL = Values.Get<uint8_t>({0, 1});

    // 1. Basic Validation (ensure ports are within physical range 0-10)
    if (!SDA.Success || !SCL.Success || SDA.Value > 10 || SCL.Value > 10)
        return false;

    // 3. Connect to Port Lists
    // This triggers PortSetup -> DriverStart inside BoardClass
    bool SdaOk = Board.Connect(this, SDA.Value);
    bool SclOk = Board.Connect(this, SCL.Value);

    if (SdaOk && SclOk)
    {
        this->CurrentSDA = SDA.Value;
        this->CurrentSCL = SCL.Value;
        return true;
    }

    // Cleanup on partial failure
    if (SdaOk)
        Board.Disconnect(this, SDA.Value);
    if (SclOk)
        Board.Disconnect(this, SCL.Value);

    return false;
}

bool I2CDeviceClass::Disconnect()
{
    I2CDriver = nullptr;

    if (CurrentSDA <= 10)
        Board.Disconnect(this, CurrentSDA);
    if (CurrentSCL <= 10)
        Board.Disconnect(this, CurrentSCL);

    CurrentSDA = 255;
    CurrentSCL = 255;
    return true;
}

void I2CDeviceClass::Setup(Path Index)
{
    // Trigger reconnection if:
    // - Full setup (Length 0)
    // - SDA changed ({0, 0})
    // - SCL changed ({0, 1})
    bool ShouldConnect = (Index.Length == 0) ||
                         (Index.Length >= 2 && Index.Indexing[0] == 0 && (Index.Indexing[1] == 0 || Index.Indexing[1] == 1));

    if (ShouldConnect)
    {
        Disconnect();
        if (!Connect())
            return;
    }

    // Hardware Initialization
    if (I2CDriver != nullptr)
    {
        I2CDevices DevType = Values.Get<I2CDevices>({0}).Value;
        uint8_t Addr = Values.Get<uint8_t>({0, 2}).Value;

        if (DevType == I2CDevices::LSM6DS3TRC)
        {
            if (Addr == 0)
            {
                Addr = 0x6A;
                Values.Set<uint8_t>(Addr, {0, 2});
            }

            // Initialize hardware
            uint8_t Config[2] = {
                0b01000100, // CTRL1_XL: 104Hz, 16g range
                0b01001100  // CTRL2_G:  104Hz, 2000dps range
            };

            // Write 2 bytes starting at 0x10 to set both registers in one go
            I2CDriver->Write(Addr, 0x10, Config, 2);
            ESP_LOGI("I2C", "LSM6DS3 initialized at 0x%02X", Addr);
            Values.Set(Vector3D(), {1, 0});
            Values.Set(Vector3D(), {1, 1});
            Values.Set<Number>(0, {1, 2});
            Values.Set<Number>(0, {1, 3});
        }
        else
        {
            Values.Delete({1, 0});
            Values.Delete({1, 1});
            Values.Delete({1, 2});
            Values.Delete({1, 3});
        }
    }
}

bool I2CDeviceClass::Run()
{
    // 1. Safety Checks
    if (I2CDriver == nullptr)
        return true;

    // Retrieve Filter values and check validity
    Getter<Number> AccFilter = Values.Get<Number>({1, 2}); // AccelerationFilter
    Getter<Number> RotFilter = Values.Get<Number>({1, 3}); // AngularFilter

    if (!AccFilter.Success || !RotFilter.Success)
        return true;

    // 2. Data Acquisition
    uint8_t Addr = Values.Get<uint8_t>({0, 2}).Value; // Use the address from Setup
    int16_t RawBuffer[6];                             // Use signed ints to handle negative G-force/rotation

    // Read 12 bytes starting at 0x22 (Gyro X-L)
    // Register map: 0x22-0x27 (Gyro), 0x28-0x2D (Accel)
    if (!I2CDriver->Read(Addr, 0x22, (uint8_t *)RawBuffer, 12))
        return true;

    // 3. Current State Retrieval
    Vector3D Acc = Values.Get<Vector3D>({1, 0}).Value; // Acceleration
    Vector3D Rot = Values.Get<Vector3D>({1, 1}).Value; // AngularRate

    // 4. Processing & Filtering
    // Weights for the Low Pass Filter: Result = New * (1-Alpha) + Old * Alpha
    // Note: In your logic, Alpha is the 'Filter' value.
    Number One = 1;
    Number AccInvWeight = One / (One + AccFilter.Value);
    Number AccWeight = AccFilter.Value / (One + AccFilter.Value);

    Number RotInvWeight = One / (One + RotFilter.Value);
    Number RotWeight = RotFilter.Value / (One + RotFilter.Value);

    Vector3D AccTemp, RotTemp;

    // Gyro Math: (Raw / 939) maps roughly to Rad/s at 2000dps range
    RotTemp.X = (Number(RawBuffer[0]) / 939) * RotInvWeight + Rot.X * RotWeight;
    RotTemp.Y = (Number(RawBuffer[1]) / 939) * RotInvWeight + Rot.Y * RotWeight;
    RotTemp.Z = (Number(RawBuffer[2]) / 939) * RotInvWeight + Rot.Z * RotWeight;

    // Accel Math: (Raw / 209) maps roughly to m/s^2 at 16g range
    AccTemp.X = (Number(RawBuffer[3]) / 209) * AccInvWeight + Acc.X * AccWeight;
    AccTemp.Y = (Number(RawBuffer[4]) / 209) * AccInvWeight + Acc.Y * AccWeight;
    AccTemp.Z = (Number(RawBuffer[5]) / 209) * AccInvWeight + Acc.Z * AccWeight;

    // 5. Update Values
    Values.Set(AccTemp, {1, 0});
    Values.Set(RotTemp, {1, 1});

    return true;
}