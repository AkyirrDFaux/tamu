class I2CDeviceClass : public BaseClass
{
public:
    ::I2C *I2CDriver = nullptr;
    PortNumber CurrentSDA = -1;
    PortNumber CurrentSCL = -1;

    I2CDeviceClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});

    void Setup(uint16_t Index);
    bool Run();

    bool Connect();
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, uint16_t Index)
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

    uint16_t cursor = 0;
    Tri ReadOnly = (this->Flags == Flags::Auto) ? Tri::Set : Tri::Reset;

    // --- Branch {0}: Hardware Configuration (Depth 0) ---
    I2CDevices initialDev = I2CDevices::Undefined;
    Values.Set(&initialDev, sizeof(I2CDevices), Types::I2CDevice, cursor++, 0, ReadOnly, Tri::Set); // {0}

    // --- Branch {0} Children (Depth 1) ---
    PortNumber negOne = -1;
    uint8_t zeroAddr = 0;

    Values.Set(&negOne, sizeof(PortNumber), Types::PortNumber, cursor++, 1, ReadOnly, Tri::Set);   // SDA {0,0}
    Values.Set(&negOne, sizeof(PortNumber), Types::PortNumber, cursor++, 1, ReadOnly, Tri::Set);   // SCL {0,1}
    Values.Set(&zeroAddr, sizeof(uint8_t), Types::Byte, cursor++, 1, ReadOnly, Tri::Set);         // Addr {0,2}

    // Branch {1} is left empty for Setup() to populate via specific sensor logic.
}

bool I2CDeviceClass::Connect()
{
    uint16_t sdaIdx = Values.Child(0);
    uint16_t sclIdx = Values.Next(sdaIdx);

    Result resSDA = Values.Get(sdaIdx);
    Result resSCL = Values.Get(sclIdx);

    if (!resSDA.Value || !resSCL.Value)
        return false;

    PortNumber sda = *(PortNumber *)resSDA.Value;
    PortNumber scl = *(PortNumber *)resSCL.Value;

    if (sda > 10 || scl > 10)
        return false;

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

void I2CDeviceClass::Setup(uint16_t Index)
{
    // Hard-indexed logic:
    // 0: Type
    // 1: SDA Port
    // 2: SCL Port
    // 3: Address

    if (Index > 3) return; // Ignore updates to the Data Group (1,x)

    // 1. Port Change: Trigger Hardware Re-link
    if (Index == 1 || Index == 2)
    {
        Disconnect();
        if (!Connect()) return;
    }

    // 2. Hardware Driver exists: Initialize sensor registers and data structure
    if (I2CDriver != nullptr)
    {
        Result typeRes = Values.Get(0);
        Result addrRes = Values.Get(3); // Direct access to {0,2} via Index 3

        if (!typeRes.Value || !addrRes.Value) return;

        I2CDevices DevType = *(I2CDevices *)typeRes.Value;
        uint8_t *AddrPtr = (uint8_t *)addrRes.Value;

        if (DevType == I2CDevices::LSM6DS3TRC)
        {
            // Default address for LSM6DS3TR-C if not set
            if (*AddrPtr == 0) *AddrPtr = 0x6A;

            // Configure IMU: Accel @ 104Hz, Gyro @ 104Hz
            uint8_t Config[2] = {0b01000100, 0b01001100};
            I2CDriver->Write(*AddrPtr, 0x10, Config, 2);

            // 3. Build Data Group {1} using Linear Cursor (starting at index 4)
            // This replaces the path-based Reference({1, x}) logic.
            Vector3D zeroVec = {0, 0, 0};
            Number zeroNum = 0.0;
            
            // Depth 0 for the Group Header, Depth 1 for Children
            // Using cursor index 4 as the start of Branch {1}
            Values.Set(nullptr, 0, Types::Undefined, 4, 0); // {1} Root
            Values.Set(&zeroVec, sizeof(Vector3D), Types::Vector3D, 5, 1); // {1,0} Accel
            Values.Set(&zeroVec, sizeof(Vector3D), Types::Vector3D, 6, 1); // {1,1} Gyro
            Values.Set(&zeroNum, sizeof(Number), Types::Number, 7, 1);    // {1,2} Temp
            Values.Set(&zeroNum, sizeof(Number), Types::Number, 8, 1);    // {1,3} Timestamp
        }
        else
        {
            // Wipe the Data Group if type becomes Undefined/Unsupported
            // Starting from index 4 (the head of branch {1})
            Values.Delete(4); 
        }
    }
}

bool I2CDeviceClass::Run()
{
    if (I2CDriver == nullptr)
        return true;

    // Direct Index Access (Zero Navigation Overhead)
    // 3: Address {0,2}
    // 5: Accel Vector {1,0}
    // 6: Gyro Vector {1,1}
    // 7: Accel Filter {1,2}
    // 8: Gyro Filter {1,3}

    Result addrRes = Values.Get(3);
    Result accVRes = Values.Get(5);
    Result rotVRes = Values.Get(6);
    
    // Safety check: if the sensor was just reconfigured, indices might be invalid
    if (!addrRes.Value || !accVRes.Value || !rotVRes.Value)
        return true;

    uint8_t Addr    = *(uint8_t *)addrRes.Value;
    Vector3D *Acc   = (Vector3D *)accVRes.Value;
    Vector3D *Rot   = (Vector3D *)rotVRes.Value;

    // Fetch filters from hard indices 7 and 8
    Result accFRes = Values.Get(7);
    Result rotFRes = Values.Get(8);
    Number AccFilter = (accFRes.Value) ? *(Number *)accFRes.Value : Number(0);
    Number RotFilter = (rotFRes.Value) ? *(Number *)rotFRes.Value : Number(0);

    // 1. Hardware I/O
    int16_t Raw[6]; // LSM6DS3 registers 0x22-0x2D (Gyro X through Accel Z)
    if (!I2CDriver->Read(Addr, 0x22, (uint8_t *)Raw, 12))
        return true;

    // 2. Pre-calculate filter weights
    // (Optimization: If filters don't change often, calculate these in Setup)
    Number AccInvW = 1.0 / (1.0 + AccFilter);
    Number AccW    = 1.0 - AccInvW;
    Number RotInvW = 1.0 / (1.0 + RotFilter);
    Number RotW    = 1.0 - RotInvW;

    // 3. Zero-Copy Update (Writing directly into the ValueTree's DataArray)
    // Sensitivity: Gyro @ 2000dps (939 LSB/dps), Accel @ 16g (209 LSB/mg)
    
    Rot->X = (Number(Raw[0]) / 939.0) * RotInvW + (Rot->X * RotW);
    Rot->Y = (Number(Raw[1]) / 939.0) * RotInvW + (Rot->Y * RotW);
    Rot->Z = (Number(Raw[2]) / 939.0) * RotInvW + (Rot->Z * RotW);

    Acc->X = (Number(Raw[3]) / 209.0) * AccInvW + (Acc->X * AccW);
    Acc->Y = (Number(Raw[4]) / 209.0) * AccInvW + (Acc->Y * AccW);
    Acc->Z = (Number(Raw[5]) / 209.0) * AccInvW + (Acc->Z * AccW);

    return true;
}