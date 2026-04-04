class I2CDeviceClass : public BaseClass
{
public:
    ::I2C *I2CDriver = nullptr;
    PortNumber CurrentSDA = -1;
    PortNumber CurrentSCL = -1;

    I2CDeviceClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});

    void Setup(const Reference &Index);
    bool Run();

    bool Connect();
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, const Reference &Index)
    {
        static_cast<I2CDeviceClass *>(Base)->Setup(Index);
    }
    static bool RunBridge(BaseClass *Base)
    {
        return static_cast<I2CDeviceClass *>(Base)->Run();
    }

    static constexpr VTable Table = {
        .Setup = I2CDeviceClass::SetupBridge,
        .Run = I2CDeviceClass::RunBridge};
};

I2CDeviceClass::I2CDeviceClass(const Reference &ID, FlagClass Flags, RunInfo Info)
    : BaseClass(&Table, ID, Flags, Info)
{
    Type = ObjectTypes::I2C;
    Name = "I2C Device";

    // 1. Root {0}: Device Type
    I2CDevices initialDev = I2CDevices::Undefined;
    Values.Set(&initialDev, sizeof(I2CDevices), Types::I2CDevice, 0);

    // 2. Configuration Group {0, x}
    PortNumber negOne = -1;
    uint8_t zeroAddr = 0;
    // Note: Child(0) is SDA, Next(SDA) is SCL, Next(SCL) is Addr
    uint16_t sdaIdx = Values.InsertChild(&negOne, sizeof(PortNumber), Types::PortNumber, 0);     // SDA {0,0}
    uint16_t sclIdx = Values.InsertNext(&negOne, sizeof(PortNumber), Types::PortNumber, sdaIdx); // SCL {0,1}
    Values.InsertNext(&zeroAddr, sizeof(uint8_t), Types::Byte, sclIdx);                          // Addr {0,2}

    // 3. Data Group {1} will be built in Setup() based on the Device Type
}

bool I2CDeviceClass::Connect()
{
    uint16_t sdaIdx = Values.Child(0);
    uint16_t sclIdx = Values.Next(sdaIdx);

    Result resSDA = Values.Get(sdaIdx);
    Result resSCL = Values.Get(sclIdx);

    if (!resSDA.Value || !resSCL.Value) return false;

    PortNumber sda = *(PortNumber*)resSDA.Value;
    PortNumber scl = *(PortNumber*)resSCL.Value;

    if (sda > 10 || scl > 10) return false;

    if (Board.ConnectI2C(this, sda, scl)) {
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
    if (Index.PathLen() > 0 && Index.Path[0] != 0) // Only setup in first group
        return;

    // Check if we need to reconnect (Port change)
    if (Index.PathLen() == 0 || (Index.PathLen() >= 2 && Index.Path[0] == 0 && Index.Path[1] < 2)) {
        Disconnect();
        if (!Connect()) return;
    }

    if (I2CDriver != nullptr) {
        // Mode is at 0, Address is the 2nd sibling of Mode's first child
        uint16_t addrIdx = Values.Next(Values.Next(Values.Child(0)));
        Result typeRes = Values.Get(0);
        Result addrRes = Values.Get(addrIdx);

        if (!typeRes.Value || !addrRes.Value) return;

        I2CDevices DevType = *(I2CDevices*)typeRes.Value;
        uint8_t* AddrPtr = (uint8_t*)addrRes.Value;

        if (DevType == I2CDevices::LSM6DS3TRC) {
            if (*AddrPtr == 0) *AddrPtr = 0x6A;
            
            uint8_t Config[2] = { 0b01000100, 0b01001100 };
            I2CDriver->Write(*AddrPtr, 0x10, Config, 2);
            
            // Build Data Group {1}
            Vector3D zeroVec = {0,0,0};
            Number zeroNum = 0;
            // Linear insertion for Group 1
            Values.Set(&zeroVec, sizeof(Vector3D), Types::Vector3D, Reference({1, 0}));
            Values.Set(&zeroVec, sizeof(Vector3D), Types::Vector3D, Reference({1, 1}));
            Values.Set(&zeroNum, sizeof(Number), Types::Number, Reference({1, 2}));
            Values.Set(&zeroNum, sizeof(Number), Types::Number, Reference({1, 3}));
        } else {
            Values.Delete(Reference({1})); // Wipe the whole data group
        }
    }
}

bool I2CDeviceClass::Run()
{
    if (I2CDriver == nullptr) return true;

    // Navigation:
    // Config: 0 -> Child -> Next -> Next (Address)
    // Data: Next(0) is Group {1}. Group 1 Child is Acc, Next is Rot, etc.
    uint16_t addrIdx = Values.Next(Values.Next(Values.Child(0)));
    uint16_t group1  = Values.Next(0); 
    
    uint16_t accVIdx = Values.Child(group1);
    uint16_t rotVIdx = Values.Next(accVIdx);
    uint16_t accFIdx = Values.Next(rotVIdx);
    uint16_t rotFIdx = Values.Next(accFIdx);

    Result addrRes = Values.Get(addrIdx);
    Result accVRes = Values.Get(accVIdx);
    Result rotVRes = Values.Get(rotVIdx);
    Result accFRes = Values.Get(accFIdx);
    Result rotFRes = Values.Get(rotFIdx);

    if (!addrRes.Value || !accVRes.Value || !rotVRes.Value) return true;

    uint8_t Addr = *(uint8_t*)addrRes.Value;
    Vector3D* Acc = (Vector3D*)accVRes.Value;
    Vector3D* Rot = (Vector3D*)rotVRes.Value;
    
    // Default filters if missing
    Number AccFilter = (accFRes.Value) ? *(Number*)accFRes.Value : Number(0.5f);
    Number RotFilter = (rotFRes.Value) ? *(Number*)rotFRes.Value : Number(0.5f);

    // Hardware I/O
    int16_t RawBuffer[6];
    if (!I2CDriver->Read(Addr, 0x22, (uint8_t *)RawBuffer, 12)) return true;

    // Filtering logic
    Number AccInvW = 1.0f / (1.0f + AccFilter);
    Number AccW    = 1.0f - AccInvW;
    Number RotInvW = 1.0f / (1.0f + RotFilter);
    Number RotW    = 1.0f - RotInvW;

    // Apply to ValueTree memory (Zero-copy)
    Rot->X = (Number(RawBuffer[0]) / 939.0f) * RotInvW + Rot->X * RotW;
    Rot->Y = (Number(RawBuffer[1]) / 939.0f) * RotInvW + Rot->Y * RotW;
    Rot->Z = (Number(RawBuffer[2]) / 939.0f) * RotInvW + Rot->Z * RotW;

    Acc->X = (Number(RawBuffer[3]) / 209.0f) * AccInvW + Acc->X * AccW;
    Acc->Y = (Number(RawBuffer[4]) / 209.0f) * AccInvW + Acc->Y * AccW;
    Acc->Z = (Number(RawBuffer[5]) / 209.0f) * AccInvW + Acc->Z * AccW;

    return true;
}