class I2CDeviceClass : public BaseClass
{
public:
    I2C *I2CDriver = nullptr;
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

    Values.Set(&negOne, sizeof(PortNumber), Types::PortNumber, cursor++, 1, ReadOnly, Tri::Set); // SDA {0,0}
    Values.Set(&negOne, sizeof(PortNumber), Types::PortNumber, cursor++, 1, ReadOnly, Tri::Set); // SCL {0,1}
    Values.Set(&zeroAddr, sizeof(uint8_t), Types::Byte, cursor++, 1, ReadOnly, Tri::Set);        // Addr {0,2}

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

    if (sda > PORT_COUNT || scl > PORT_COUNT)
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
    if (Index > 3)
        return;

    // 1. Port Change: Trigger Hardware Re-link
    if (Index == 1 || Index == 2)
    {
        Disconnect();
        if (!Connect())
            return;
    }

    if (I2CDriver != nullptr)
    {
        Result typeRes = Values.Get(0);
        Result addrRes = Values.Get(3);

        if (!typeRes.Value || !addrRes.Value)
            return;

        I2CDevices DevType = *(I2CDevices *)typeRes.Value;
        uint8_t *AddrPtr = (uint8_t *)addrRes.Value;

        // These are only used if an IMU is configured
        Vector3D zeroVec = {0, 0, 0};
        Number zeroNum = N(0.0);

// --- LSM6DS3TR-C Implementation ---
#if defined O_I2C_LSM6DS3TRC
        if (DevType == I2CDevices::LSM6DS3TRC)
        {
            if (*AddrPtr == 0)
                *AddrPtr = 0x6A;

            uint8_t Config[2] = {0b01000100, 0b01001100};
            I2CDriver->Write(*AddrPtr, 0x10, Config, 2);

            Values.Set(nullptr, 0, Types::Undefined, 4, 0);
            Values.Set(&zeroVec, sizeof(Vector3D), Types::Vector3D, 5, 1);
            Values.Set(&zeroVec, sizeof(Vector3D), Types::Vector3D, 6, 1);
            Values.Set(&zeroNum, sizeof(Number), Types::Number, 7, 1);
            Values.Set(&zeroNum, sizeof(Number), Types::Number, 8, 1);
            return; // Early exit to save jumping through other checks
        }
#endif

// --- BMI160 Implementation ---
#if defined O_I2C_BMI160
        if (DevType == I2CDevices::BMI160)
        {
            if (*AddrPtr == 0)
                *AddrPtr = 0x69;

            uint8_t pwrAcc = 0x11;
            I2CDriver->Write(*AddrPtr, 0x7E, &pwrAcc, 1);
            HW::SleepMicro(5000);

            uint8_t pwrGyr = 0x15;
            I2CDriver->Write(*AddrPtr, 0x7E, &pwrGyr, 1);
            HW::SleepMicro(10000);

            uint8_t Config[2] = {0x28, 0x28};
            I2CDriver->Write(*AddrPtr, 0x40, Config, 2);

            Values.Set(nullptr, 0, Types::Undefined, 4, 0);
            Values.Set(&zeroVec, sizeof(Vector3D), Types::Vector3D, 5, 1);
            Values.Set(&zeroVec, sizeof(Vector3D), Types::Vector3D, 6, 1);
            Values.Set(&zeroNum, sizeof(Number), Types::Number, 7, 1);
            Values.Set(&zeroNum, sizeof(Number), Types::Number, 8, 1);
            return;
        }
#endif

        // Default: If no drivers matched (or were compiled in)
        Values.Delete(4);
    }
}

bool I2CDeviceClass::Run()
{
    if (I2CDriver == nullptr)
        return true;

    Result typeRes = Values.Get(0);
    Result addrRes = Values.Get(3);
    Result accVRes = Values.Get(5);
    Result rotVRes = Values.Get(6);

    if (!typeRes.Value || !addrRes.Value || !accVRes.Value || !rotVRes.Value)
        return true;

    I2CDevices DevType = *(I2CDevices *)typeRes.Value;
    uint8_t Addr = *(uint8_t *)addrRes.Value;
    Vector3D *Acc = (Vector3D *)accVRes.Value;
    Vector3D *Rot = (Vector3D *)rotVRes.Value;

    Result accFRes = Values.Get(7);
    Result rotFRes = Values.Get(8);
    Number AccFilter = (accFRes.Value) ? *(Number *)accFRes.Value : Number(0);
    Number RotFilter = (rotFRes.Value) ? *(Number *)rotFRes.Value : Number(0);

    int16_t Raw[6];
    Number accSens, gyrSens;

    bool processed = false;

// --- LSM6DS3TR-C ---
#if defined O_I2C_LSM6DS3TRC
    if (!processed && DevType == I2CDevices::LSM6DS3TRC)
    {
        if (!I2CDriver->Read(Addr, 0x22, (uint8_t *)Raw, 12))
            return true;
        gyrSens = N(939.0);
        accSens = N(209.0);
        processed = true;
    }
#endif

// --- BMI160 ---
#if defined O_I2C_BMI160
    if (!processed && DevType == I2CDevices::BMI160)
    {
        static bool identified = false;
        if (!identified)
        {
            uint8_t chipID = 0;
            if (!I2CDriver->Read(Addr, 0x00, &chipID, 1))
                return true;
            identified = true;
        }

        if (!I2CDriver->Read(Addr, 0x0C, (uint8_t *)&Raw[0], 6))
            return true;
        if (!I2CDriver->Read(Addr, 0x12, (uint8_t *)&Raw[3], 6))
            return true;

        accSens = N(939.0);
        gyrSens = N(209.0);
        processed = true;
    }
#endif

    // Final Fallback
    if (!processed)
        return true;

    // Filter Logic
    Number AccInvW = 1.0 / (1.0 + AccFilter);
    Number AccW = 1.0 - AccInvW;
    Number RotInvW = 1.0 / (1.0 + RotFilter);
    Number RotW = 1.0 - RotInvW;

    Rot->X = (Number(Raw[0]) / gyrSens) * RotInvW + (Rot->X * RotW);
    Rot->Y = (Number(Raw[1]) / gyrSens) * RotInvW + (Rot->Y * RotW);
    Rot->Z = (Number(Raw[2]) / gyrSens) * RotInvW + (Rot->Z * RotW);

    Acc->X = (Number(Raw[3]) / accSens) * AccInvW + (Acc->X * AccW);
    Acc->Y = (Number(Raw[4]) / accSens) * AccInvW + (Acc->Y * AccW);
    Acc->Z = (Number(Raw[5]) / accSens) * AccInvW + (Acc->Z * AccW);

    return true;
}