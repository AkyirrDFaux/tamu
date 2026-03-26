class I2CDeviceClass : public BaseClass
{
public:
    I2C *I2CDriver = nullptr;
    uint8_t CurrentSDA = 255;
    uint8_t CurrentSCL = 255;

    // Updated to use Reference
    void Setup(const Reference &Index); 
    I2CDeviceClass(const Reference &ID, ObjectInfo Info = {Flags::None, 1});
    bool Run();

    bool Connect();
    bool Disconnect();

    // Bridges updated for Reference
    static void SetupBridge(BaseClass *Base, const Reference &Index) { 
        static_cast<I2CDeviceClass *>(Base)->Setup(Index); 
    }
    static bool RunBridge(BaseClass *Base) { 
        return static_cast<I2CDeviceClass *>(Base)->Run(); 
    }
    
    static constexpr VTable Table = {
        .Setup = I2CDeviceClass::SetupBridge,
        .Run = I2CDeviceClass::RunBridge
    };
};

I2CDeviceClass::I2CDeviceClass(const Reference &ID, ObjectInfo Info) : BaseClass(&Table, ID, Info)
{
    Type = ObjectTypes::I2C;
    Name = "I2C Device";

    // Initializer lists {} now automatically create Local References
    Values.Set(I2CDevices::Undefined, {0});
    Values.Set<uint8_t>(255, {0, 0}); // SDA Port Index
    Values.Set<uint8_t>(255, {0, 1}); // SCL Port Index
    Values.Set<uint8_t>(0,   {0, 2}); // I2C Address
};

bool I2CDeviceClass::Connect()
{
    Getter<uint8_t> SDA = Values.Get<uint8_t>({0, 0});
    Getter<uint8_t> SCL = Values.Get<uint8_t>({0, 1});

    if (!SDA.Success || !SCL.Success || SDA.Value > 10 || SCL.Value > 10)
        return false;

    // Board.Connect now takes 'this' and uses the Registry to find our Global ID
    bool SdaOk = Board.Connect(this, SDA.Value);
    bool SclOk = Board.Connect(this, SCL.Value);

    if (SdaOk && SclOk)
    {
        this->CurrentSDA = SDA.Value;
        this->CurrentSCL = SCL.Value;
        return true;
    }

    if (SdaOk) Board.Disconnect(this, SDA.Value);
    if (SclOk) Board.Disconnect(this, SCL.Value);

    return false;
}

bool I2CDeviceClass::Disconnect()
{
    I2CDriver = nullptr;

    if (CurrentSDA <= 10) Board.Disconnect(this, CurrentSDA);
    if (CurrentSCL <= 10) Board.Disconnect(this, CurrentSCL);

    CurrentSDA = 255;
    CurrentSCL = 255;
    return true;
}

void I2CDeviceClass::Setup(const Reference &Index)
{
    // Full refresh if PathLen is 0
    bool ShouldConnect = (Index.PathLen() == 0);

    // Check Path array to see if specific configuration values changed
    if (!ShouldConnect && Index.PathLen() >= 2 && Index.Path[0] == 0)
    {
        // If SDA {0,0} or SCL {0,1} changed, we must re-bind to the Board
        if (Index.Path[1] == 0 || Index.Path[1] == 1)
            ShouldConnect = true;
    }

    if (ShouldConnect)
    {
        Disconnect();
        if (!Connect()) return;
    }

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
            uint8_t Config[2] = { 0b01000100, 0b01001100 };
            I2CDriver->Write(Addr, 0x10, Config, 2);
            
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
    if (I2CDriver == nullptr) return true;

    Getter<Number> AccFilter = Values.Get<Number>({1, 2});
    Getter<Number> RotFilter = Values.Get<Number>({1, 3});

    if (!AccFilter.Success || !RotFilter.Success) return true;

    uint8_t Addr = Values.Get<uint8_t>({0, 2}).Value;
    int16_t RawBuffer[6];

    if (!I2CDriver->Read(Addr, 0x22, (uint8_t *)RawBuffer, 12))
        return true;

    Vector3D Acc = Values.Get<Vector3D>({1, 0}).Value;
    Vector3D Rot = Values.Get<Vector3D>({1, 1}).Value;

    // Math remains the same; Reference-based Gets are now bit-field optimized
    Number One = 1;
    Number AccInvWeight = One / (One + AccFilter.Value);
    Number AccWeight = AccFilter.Value / (One + AccFilter.Value);

    Number RotInvWeight = One / (One + RotFilter.Value);
    Number RotWeight = RotFilter.Value / (One + RotFilter.Value);

    Vector3D AccTemp, RotTemp;

    RotTemp.X = (Number(RawBuffer[0]) / 939) * RotInvWeight + Rot.X * RotWeight;
    RotTemp.Y = (Number(RawBuffer[1]) / 939) * RotInvWeight + Rot.Y * RotWeight;
    RotTemp.Z = (Number(RawBuffer[2]) / 939) * RotInvWeight + Rot.Z * RotWeight;

    AccTemp.X = (Number(RawBuffer[3]) / 209) * AccInvWeight + Acc.X * AccWeight;
    AccTemp.Y = (Number(RawBuffer[4]) / 209) * AccInvWeight + Acc.Y * AccWeight;
    AccTemp.Z = (Number(RawBuffer[5]) / 209) * AccInvWeight + Acc.Z * AccWeight;

    Values.Set(AccTemp, {1, 0});
    Values.Set(RotTemp, {1, 1});

    return true;
}