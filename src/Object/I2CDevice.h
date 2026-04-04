class I2CDeviceClass : public BaseClass
{
public:
    ::I2C *I2CDriver = nullptr;
    PortNumber CurrentSDA = -1;
    PortNumber CurrentSCL = -1;

    I2CDeviceClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1,0});
    
    void Setup(const Reference &Index); 
    bool Run();

    bool Connect();
    bool Disconnect();

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

I2CDeviceClass::I2CDeviceClass(const Reference &ID, FlagClass Flags, RunInfo Info) : BaseClass(&Table, ID, Flags, Info)
{
    Type = ObjectTypes::I2C;
    Name = "I2C Device";

    I2CDevices initialDev = I2CDevices::Undefined;
    Values.Set(&initialDev, sizeof(I2CDevices), Types::I2CDevice, Reference({0}));

    PortNumber negOne = -1;
    Values.Set(&negOne, sizeof(PortNumber), Types::PortNumber, Reference({0, 0})); // SDA
    Values.Set(&negOne, sizeof(PortNumber), Types::PortNumber, Reference({0, 1})); // SCL

    uint8_t zeroAddr = 0;
    Values.Set(&zeroAddr, sizeof(uint8_t), Types::Byte, Reference({0, 2})); // Address
};

bool I2CDeviceClass::Connect()
{
    Bookmark sdaMark = Values.Find({0, 0}, true);
    Bookmark sclMark = Values.Find({0, 1}, true);

    Result resSDA = Values.Get(sdaMark);
    Result resSCL = Values.Get(sclMark);

    if (!resSDA.Value || !resSCL.Value) return false;

    PortNumber sda = *(PortNumber*)resSDA.Value;
    PortNumber scl = *(PortNumber*)resSCL.Value;

    if (sda > 10 || scl > 10) return false;

    if (Board.ConnectI2C(this, sda, scl))
    {
        this->CurrentSDA = sda;
        this->CurrentSCL = scl;
        return true;
    }

    return false;
}

bool I2CDeviceClass::Disconnect()
{
    I2CDriver = nullptr;
    Board.DisconnectI2C(this, CurrentSDA, CurrentSCL);
    CurrentSDA = -1;
    CurrentSCL = -1;
    return true;
}

void I2CDeviceClass::Setup(const Reference &Index)
{
    bool ShouldConnect = (Index.PathLen() == 0);

    if (!ShouldConnect && Index.PathLen() >= 2 && Index.Path[0] == 0)
    {
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
        Bookmark typeMark = Values.Find({0}, true);
        Bookmark addrMark = Values.Find({0, 2}, true);

        Result typeRes = Values.Get(typeMark);
        Result addrRes = Values.Get(addrMark);

        if (!typeRes.Value || !addrRes.Value) return;

        I2CDevices DevType = *(I2CDevices*)typeRes.Value;
        uint8_t* AddrPtr = (uint8_t*)addrRes.Value;

        if (DevType == I2CDevices::LSM6DS3TRC)
        {
            if (*AddrPtr == 0) *AddrPtr = 0x6A;
            
            uint8_t Config[2] = { 0b01000100, 0b01001100 };
            I2CDriver->Write(*AddrPtr, 0x10, Config, 2);
            
            Vector3D zeroVec = {0,0,0};
            Number zeroNum = 0;
            Values.Set(&zeroVec, sizeof(Vector3D), Types::Vector3D, {1, 0});
            Values.Set(&zeroVec, sizeof(Vector3D), Types::Vector3D, {1, 1});
            Values.Set(&zeroNum, sizeof(Number), Types::Number, {1, 2});
            Values.Set(&zeroNum, sizeof(Number), Types::Number, {1, 3});
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

    // 1. Fetch Bookmarks
    Bookmark addrMark = Values.Find({0, 2}, true);
    Bookmark accVMark = Values.Find({1, 0}, true);
    Bookmark rotVMark = Values.Find({1, 1}, true);
    Bookmark accFMark = Values.Find({1, 2}, true);
    Bookmark rotFMark = Values.Find({1, 3}, true);

    // 2. Resolve Results
    Result addrRes = Values.Get(addrMark);
    Result accFRes = Values.Get(accFMark);
    Result rotFRes = Values.Get(rotFMark);
    Result accVRes = Values.Get(accVMark);
    Result rotVRes = Values.Get(rotVMark);

    if (!addrRes.Value || !accFRes.Value || !rotFRes.Value || !accVRes.Value || !rotVRes.Value) 
        return true;

    uint8_t Addr = *(uint8_t*)addrRes.Value;
    Number AccFilter = *(Number*)accFRes.Value;
    Number RotFilter = *(Number*)rotFRes.Value;
    Vector3D* Acc = (Vector3D*)accVRes.Value;
    Vector3D* Rot = (Vector3D*)rotVRes.Value;

    // 3. Hardware I/O
    int16_t RawBuffer[6];
    if (!I2CDriver->Read(Addr, 0x22, (uint8_t *)RawBuffer, 12)) return true;

    // 4. Zero-Copy Signal Processing
    Number One = 1.0f;
    Number AccInvW = One / (One + AccFilter);
    Number AccW    = AccFilter * AccInvW;

    Number RotInvW = One / (One + RotFilter);
    Number RotW    = RotFilter * RotInvW;

    // Gyro filtering
    Rot->X = (Number(RawBuffer[0]) / 939.0f) * RotInvW + Rot->X * RotW;
    Rot->Y = (Number(RawBuffer[1]) / 939.0f) * RotInvW + Rot->Y * RotW;
    Rot->Z = (Number(RawBuffer[2]) / 939.0f) * RotInvW + Rot->Z * RotW;

    // Accel filtering
    Acc->X = (Number(RawBuffer[3]) / 209.0f) * AccInvW + Acc->X * AccW;
    Acc->Y = (Number(RawBuffer[4]) / 209.0f) * AccInvW + Acc->Y * AccW;
    Acc->Z = (Number(RawBuffer[5]) / 209.0f) * AccInvW + Acc->Z * AccW;

    return true;
}