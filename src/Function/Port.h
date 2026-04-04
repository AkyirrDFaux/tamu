void BoardClass::Setup(const Reference &Index)
{
#if defined BOARD_Tamu_v2_0
    // Initialize onboard devices on root setup
    if (Index.PathLen() == 0)
    {
        // Reference(0,2,0) points to the local Gyro/Acc object
        I2CDeviceClass *GyroAcc = new I2CDeviceClass(Reference::Global(0, 2, 0), Flags::Auto | Flags::RunLoop);

        PortNumber sda = 8, scl = 9;
        GyroAcc->ValueSetup(&sda, sizeof(PortNumber), Types::PortNumber, Reference({0, 0}));
        GyroAcc->ValueSetup(&scl, sizeof(PortNumber), Types::PortNumber, Reference({0, 1}));

        I2CDevices model = I2CDevices::LSM6DS3TRC;
        GyroAcc->ValueSetup(&model, sizeof(I2CDevices), Types::I2CDevice, Reference({0}));

        InputClass *Button = new InputClass(Reference::Global(0, 2, 1), Flags::Auto | Flags::RunLoop);
        PortNumber btnPin = 10;
        Button->ValueSetup(&btnPin, sizeof(PortNumber), Types::PortNumber, Reference({0, 0}));

        Inputs inType = Inputs::ButtonWithLED;
        Button->ValueSetup(&inType, sizeof(Inputs), Types::Input, Reference({0}));
    }
#endif
}

// --- PIN CONNECTIONS ---

bool BoardClass::ConnectPin(BaseClass *Object, PortNumber Port)
{
    if (Object == nullptr || Port > 10) return false;

    if (Object->Type != ObjectTypes::Input &&
        Object->Type != ObjectTypes::Sensor &&
        Object->Type != ObjectTypes::Output)
        return false;

    Reference ID = Objects.GetReference(Object);
    if (!ID.IsValid()) return false;

    // Check if slot {0, 0, Port, 1, 0} is already taken
    if (Values.Find({0, 0, Port, 1, 0}, true).Index != 0xFFFF)
        return false;

    // Set uses Reference overload to find/insert path automatically
    Values.Set(&ID, sizeof(Reference), Types::Reference, {0, 0, Port, 1, 0});
    
    DriverPin(Port);
    return true;
}

void BoardClass::DriverPin(PortNumber Port)
{
    Bookmark pinMark = Values.Find({0, 0, Port, 0}, true);
    Bookmark refMark = Values.Find({0, 0, Port, 1, 0}, true);

    if (pinMark.Index == 0xFFFF || refMark.Index == 0xFFFF)
    {
        Drivers none = Drivers::None;
        Values.Set(&none, sizeof(Drivers), Types::PortDriver, {0, 0, Port, 1});
        return;
    }

    Result pinRes = Values.Get(pinMark);
    Result refRes = Values.Get(refMark);

    ::Pin PortPin = *(::Pin *)pinRes.Value;
    BaseClass *Obj = Objects.At(*(Reference *)refRes.Value);
    if (!Obj) return;

    Drivers role = Drivers::None;

    if (Obj->Type == ObjectTypes::Input)
    {
        role = Drivers::Input;
        static_cast<InputClass *>(Obj)->InputPin = PortPin;
    }
    else if (Obj->Type == ObjectTypes::Sensor)
    {
        role = Drivers::Analog;
        static_cast<SensorClass *>(Obj)->MeasPin = PortPin;
    }
    else if (Obj->Type == ObjectTypes::Output)
    {
        role = Drivers::Output;
        static_cast<OutputClass *>(Obj)->PWMPin = PortPin;
    }

    if (role != Drivers::None)
    {
        Values.Set(&role, sizeof(Drivers), Types::PortDriver, {0, 0, Port, 1});
    }
}

bool BoardClass::DisconnectPin(BaseClass *Object, PortNumber Port)
{
    if (Object == nullptr || Port > 10) return false;

    Reference ID = Objects.GetReference(Object);
    Bookmark refMark = Values.Find({0, 0, Port, 1, 0}, true);
    Result refRes = Values.Get(refMark);

    if (refRes.Value && *(Reference *)refRes.Value == ID)
    {
        if (Object->Type == ObjectTypes::Input)
            static_cast<InputClass *>(Object)->InputPin = INVALID_PIN;
        else if (Object->Type == ObjectTypes::Sensor)
            static_cast<SensorClass *>(Object)->MeasPin = INVALID_PIN;
        else if (Object->Type == ObjectTypes::Output)
            static_cast<OutputClass *>(Object)->PWMPin = INVALID_PIN;

        Values.Delete({0, 0, Port, 1, 0});
        Drivers none = Drivers::None;
        Values.Set(&none, sizeof(Drivers), Types::PortDriver, {0, 0, Port, 1});
        return true;
    }
    return false;
}

// --- LED / DISPLAY CONNECTIONS ---

bool BoardClass::ConnectLED(BaseClass *Object, PortNumber Port, uint8_t Index)
{
    if (Object == nullptr || Port > 10 || Object->Type != ObjectTypes::Display)
        return false;

    Reference ID = Objects.GetReference(Object);
    if (!ID.IsValid()) return false;

    if (Values.Find({0, 0, Port, 1, Index}, true).Index != 0xFFFF)
        return false;

    Values.Set(&ID, sizeof(Reference), Types::Reference, {0, 0, Port, 1, Index});
    DriverLED(Port);
    return true;
}

void BoardClass::DriverLED(PortNumber Port)
{
    Bookmark pinMark = Values.Find({0, 0, Port, 0}, true);
    if (pinMark.Index == 0xFFFF) return;
    ::Pin PortPin = *(::Pin *)Values.Get(pinMark).Value;

    uint32_t TotalLength = 0;
    uint8_t y = 0;

    // 1. Calculate combined length
    while (true)
    {
        Bookmark refMark = Values.Find({0, 0, Port, 1, y}, true);
        if (refMark.Index == 0xFFFF) break;

        BaseClass *Obj = Objects.At(*(Reference *)Values.Get(refMark).Value);
        if (Obj && Obj->Type == ObjectTypes::Display)
        {
            Result lenRes = Obj->Values.Get(Obj->Values.Find({0, 1}, true));
            if (lenRes.Value) TotalLength += *(int32_t *)lenRes.Value;
        }
        y++;
    }

    if (TotalLength > 0)
    {
        if (DriverArray[Port]) delete static_cast<LEDDriver *>(DriverArray[Port]);

        LEDDriver *NewDriver = new LEDDriver(TotalLength, PortPin);
        DriverArray[Port] = NewDriver;

        Drivers role = Drivers::LED;
        Values.Set(&role, sizeof(Drivers), Types::PortDriver, {0, 0, Port, 1});

        // 2. Distribute segment offsets
        uint32_t CurrentOffset = 0;
        for (uint8_t i = 0; i < y; i++)
        {
            Bookmark segMark = Values.Find({0, 0, Port, 1, i}, true);
            DisplayClass *Disp = static_cast<DisplayClass *>(Objects.At(*(Reference *)Values.Get(segMark).Value));
            if (Disp)
            {
                Disp->LEDs = NewDriver->Offset(CurrentOffset);
                Result subLen = Disp->Values.Get(Disp->Values.Find({0, 1}, true));
                if (subLen.Value) CurrentOffset += *(int32_t *)subLen.Value;
            }
        }
    }
}

bool BoardClass::DisconnectLED(BaseClass *Object, PortNumber Port)
{
    if (Object == nullptr || Port > 10) return false;
    Reference ID = Objects.GetReference(Object);
    uint8_t y = 0;
    bool found = false;

    while (y < 32)
    {
        Bookmark resMark = Values.Find({0, 0, Port, 1, y}, true);
        if (resMark.Index == 0xFFFF) break;

        if (*(Reference *)Values.Get(resMark).Value == ID)
        {
            static_cast<DisplayClass *>(Object)->LEDs = nullptr;
            Values.Delete({0, 0, Port, 1, y});
            found = true;

            // Shift contiguous list
            uint8_t next = y + 1;
            while (true)
            {
                Bookmark upMark = Values.Find({0, 0, Port, 1, next}, true);
                if (upMark.Index == 0xFFFF) break;

                Reference upRef = *(Reference *)Values.Get(upMark).Value;
                Values.Set(&upRef, sizeof(Reference), Types::Reference, {0, 0, Port, 1, (uint8_t)(next - 1)});
                Values.Delete({0, 0, Port, 1, next});
                next++;
            }
            break;
        }
        y++;
    }

    if (found)
    {
        if (Values.Find({0, 0, Port, 1, 0}, true).Index == 0xFFFF)
        {
            if (DriverArray[Port]) delete static_cast<LEDDriver *>(DriverArray[Port]);
            DriverArray[Port] = nullptr;
            Drivers none = Drivers::None;
            Values.Set(&none, sizeof(Drivers), Types::PortDriver, {0, 0, Port, 1});
        }
        else DriverLED(Port);
        
        return true;
    }
    return false;
}

// --- I2C CONNECTIONS ---

bool BoardClass::ConnectI2C(BaseClass *Object, PortNumber SDA, PortNumber SCL)
{
    if (Object == nullptr || SDA > 10 || SCL > 10 || Object->Type != ObjectTypes::I2C)
        return false;

    Reference ID = Objects.GetReference(Object);
    if (!ID.IsValid()) return false;

    uint8_t y = 0;
    while (y < 32)
    {
        Bookmark resMark = Values.Find({0, 0, SDA, 1, y}, true);
        if (resMark.Index == 0xFFFF) break;
        if (*(Reference *)Values.Get(resMark).Value == ID) return true; 
        y++;
    }

    Values.Set(&ID, sizeof(Reference), Types::Reference, {0, 0, SDA, 1, y});
    Values.Set(&SCL, sizeof(PortNumber), Types::PortNumber, {0, 0, SDA, 2});
    Values.Set(&SDA, sizeof(PortNumber), Types::PortNumber, {0, 0, SCL, 2});

    DriverI2C(SDA, SCL);
    return true;
}

void BoardClass::DriverI2C(PortNumber SDA, PortNumber SCL)
{
    if (DriverArray[SDA] == nullptr)
    {
        Bookmark sdaMark = Values.Find({0, 0, SDA, 0}, true);
        Bookmark sclMark = Values.Find({0, 0, SCL, 0}, true);

        if (sdaMark.Index != 0xFFFF && sclMark.Index != 0xFFFF)
        {
            ::I2C *Bus = new ::I2C();
            if (Bus->Begin(*(::Pin *)Values.Get(sclMark).Value, *(::Pin *)Values.Get(sdaMark).Value, 400000))
            {
                DriverArray[SDA] = Bus;
                DriverArray[SCL] = Bus;
                Drivers dSDA = Drivers::I2C_SDA, dSCL = Drivers::I2C_SCL;
                Values.Set(&dSDA, sizeof(Drivers), Types::PortDriver, {0, 0, SDA, 1});
                Values.Set(&dSCL, sizeof(Drivers), Types::PortDriver, {0, 0, SCL, 1});
            }
            else { delete Bus; return; }
        }
    }

    ::I2C *ActiveBus = static_cast<::I2C *>(DriverArray[SDA]);
    uint8_t y = 0;
    while (true)
    {
        Bookmark devMark = Values.Find({0, 0, SDA, 1, y++}, true);
        if (devMark.Index == 0xFFFF) break;

        I2CDeviceClass *Dev = static_cast<I2CDeviceClass *>(Objects.At(*(Reference *)Values.Get(devMark).Value));
        if (Dev) Dev->I2CDriver = ActiveBus;
    }
}

bool BoardClass::DisconnectI2C(BaseClass *Object, PortNumber SDA, PortNumber SCL)
{
    if (!Object || SDA > 10 || SCL > 10) return false;
    Reference ID = Objects.GetReference(Object);
    uint8_t y = 0;
    bool found = false;

    while (y < 32)
    {
        Bookmark resMark = Values.Find({0, 0, SDA, 1, y}, true);
        if (resMark.Index == 0xFFFF) break;

        if (*(Reference *)Values.Get(resMark).Value == ID)
        {
            static_cast<I2CDeviceClass *>(Object)->I2CDriver = nullptr;
            Values.Delete({0, 0, SDA, 1, y});
            found = true;

            uint8_t next = y + 1;
            while (true)
            {
                Bookmark upMark = Values.Find({0, 0, SDA, 1, next}, true);
                if (upMark.Index == 0xFFFF) break;

                Reference upRef = *(Reference *)Values.Get(upMark).Value;
                Values.Set(&upRef, sizeof(Reference), Types::Reference, {0, 0, SDA, 1, (uint8_t)(next - 1)});
                Values.Delete({0, 0, SDA, 1, next});
                next++;
            }
            break;
        }
        y++;
    }

    if (found && Values.Find({0, 0, SDA, 1, 0}, true).Index == 0xFFFF)
    {
        if (DriverArray[SDA]) delete static_cast<::I2C *>(DriverArray[SDA]);
        DriverArray[SDA] = nullptr;
        DriverArray[SCL] = nullptr;

        Drivers none = Drivers::None;
        Values.Set(&none, sizeof(Drivers), Types::PortDriver, {0, 0, SDA, 1});
        Values.Set(&none, sizeof(Drivers), Types::PortDriver, {0, 0, SCL, 1});
        Values.Delete({0, 0, SDA, 2});
        Values.Delete({0, 0, SCL, 2});
    }

    return found;
}