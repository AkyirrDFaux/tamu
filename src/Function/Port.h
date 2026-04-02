void BoardClass::Setup(const Reference &Index)
{
#if defined BOARD_Tamu_v2_0
    // Initialize onboard devices on root setup
    if (Index.PathLen() == 0)
    {
        // Reference(0,2,0) points to the local Gyro/Acc object
        I2CDeviceClass *GyroAcc = new I2CDeviceClass(Reference::Global(0, 2, 0), Flags::Auto | Flags::RunLoop);

        PortNumber sda = 8, scl = 9;
        GyroAcc->ValueSetup(&sda, sizeof(PortNumber), Types::PortNumber, {0, 0});
        GyroAcc->ValueSetup(&scl, sizeof(PortNumber), Types::PortNumber, {0, 1});

        I2CDevices model = I2CDevices::LSM6DS3TRC;
        GyroAcc->ValueSetup(&model, sizeof(I2CDevices), Types::I2CDevice, {0});

        InputClass *Button = new InputClass(Reference::Global(0, 2, 1), Flags::Auto | Flags::RunLoop);
        PortNumber btnPin = 10;
        Button->ValueSetup(&btnPin, sizeof(PortNumber), Types::PortNumber, {0, 0});

        Inputs inType = Inputs::ButtonWithLED;
        Button->ValueSetup(&inType, sizeof(Inputs), Types::Input, {0});
    }
#endif
}

bool BoardClass::ConnectPin(BaseClass *Object, PortNumber Port)
{
    if (Object == nullptr || Port > 10)
        return false;

    // Validate object compatibility
    if (Object->Type != ObjectTypes::Input &&
        Object->Type != ObjectTypes::Sensor &&
        Object->Type != ObjectTypes::Output)
        return false;

    Reference ID = Objects.GetReference(Object);
    if (!ID.IsValid())
        return false;

    // Check if slot {0, 0, Port, 1, 0} is already taken
    if (Values.Find({0, 0, Port, 1, 0}, true).Value)
        return false;

    Values.Set(&ID, sizeof(Reference), Types::Reference, {0, 0, Port, 1, 0});
    DriverPin(Port);
    return true;
}

void BoardClass::DriverPin(PortNumber Port)
{
    SearchResult pinRes = Values.Find({0, 0, Port, 0}, true);
    SearchResult refRes = Values.Find({0, 0, Port, 1, 0}, true);

    if (!pinRes.Value || !refRes.Value)
    {
        Drivers none = Drivers::None;
        Values.Set(&none, sizeof(Drivers), Types::PortDriver, {0, 0, Port, 1});
        return;
    }

    ::Pin PortPin = *(::Pin *)pinRes.Value;
    BaseClass *Obj = Objects.At(*(Reference *)refRes.Value);
    if (!Obj)
        return;

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
    if (Object == nullptr || Port > 10)
        return false;

    Reference ID = Objects.GetReference(Object);
    SearchResult refRes = Values.Find({0, 0, Port, 1, 0}, true);

    if (refRes.Value && *(Reference *)refRes.Value == ID)
    {
        // Reset the object's internal hardware pointer
        if (Object->Type == ObjectTypes::Input)
            static_cast<InputClass *>(Object)->InputPin = INVALID_PIN;
        else if (Object->Type == ObjectTypes::Sensor)
            static_cast<SensorClass *>(Object)->MeasPin = INVALID_PIN;
        else if (Object->Type == ObjectTypes::Output)
            static_cast<OutputClass *>(Object)->PWMPin = INVALID_PIN;

        // Wipe the registry and reset driver status
        Values.Delete({0, 0, Port, 1, 0});
        Drivers none = Drivers::None;
        Values.Set(&none, sizeof(Drivers), Types::PortDriver, {0, 0, Port, 1});
        return true;
    }
    return false;
}

bool BoardClass::ConnectLED(BaseClass *Object, PortNumber Port, uint8_t Index)
{
    if (Object == nullptr || Port > 10 || Object->Type != ObjectTypes::Display)
        return false;

    Reference ID = Objects.GetReference(Object);
    if (!ID.IsValid())
        return false;

    // Check if this slot is already occupied
    if (Values.Find({0, 0, Port, 1, Index}, true).Value)
        return false;

    Values.Set(&ID, sizeof(Reference), Types::Reference, {0, 0, Port, 1, Index});
    DriverLED(Port);
    return true;
}

void BoardClass::DriverLED(PortNumber Port)
{
    SearchResult pinRes = Values.Find({0, 0, Port, 0}, true);
    if (!pinRes.Value)
        return;
    ::Pin PortPin = *(::Pin *)pinRes.Value;

    uint32_t TotalLength = 0;
    uint8_t y = 0;

    // 1. Calculate combined length of all Displays in the chain
    while (true)
    {
        SearchResult refRes = Values.Find({0, 0, Port, 1, y}, true);
        if (!refRes.Value)
            break;

        BaseClass *Obj = Objects.At(*(Reference *)refRes.Value);
        if (Obj && Obj->Type == ObjectTypes::Display)
        {
            SearchResult lenRes = Obj->Values.Find({0, 1}, true);
            if (lenRes.Value)
                TotalLength += *(int32_t *)lenRes.Value;
        }
        y++;
    }

    // 2. Refresh the Hardware Driver
    if (TotalLength > 0)
    {
        if (DriverArray[Port])
            delete static_cast<LEDDriver *>(DriverArray[Port]);

        LEDDriver *NewDriver = new LEDDriver(TotalLength, PortPin);
        DriverArray[Port] = NewDriver;

        Drivers role = Drivers::LED;
        Values.Set(&role, sizeof(Drivers), Types::PortDriver, {0, 0, Port, 1});

        // 3. Distribute pointers to each Display segment
        uint32_t CurrentOffset = 0;
        for (uint8_t i = 0; i < y; i++)
        {
            SearchResult refRes = Values.Find({0, 0, Port, 1, i}, true);
            DisplayClass *Disp = static_cast<DisplayClass *>(Objects.At(*(Reference *)refRes.Value));
            if (Disp)
            {
                Disp->LEDs = NewDriver->Offset(CurrentOffset);
                SearchResult subLen = Disp->Values.Find({0, 1}, true);
                if (subLen.Value)
                    CurrentOffset += *(int32_t *)subLen.Value;
            }
        }
    }
}

bool BoardClass::DisconnectLED(BaseClass *Object, PortNumber Port)
{
    if (Object == nullptr || Port > 10)
        return false;
    Reference ID = Objects.GetReference(Object);
    uint8_t y = 0;
    bool found = false;

    while (y < 32)
    {
        SearchResult res = Values.Find({0, 0, Port, 1, y}, true);
        if (!res.Value)
            break;

        if (*(Reference *)res.Value == ID)
        {
            static_cast<DisplayClass *>(Object)->LEDs = nullptr;
            Values.Delete({0, 0, Port, 1, y});
            found = true;

            // Shift subsequent references to keep the daisy-chain contiguous
            uint8_t next = y + 1;
            while (true)
            {
                SearchResult up = Values.Find({0, 0, Port, 1, next}, true);
                if (!up.Value)
                    break;

                // Move reference up one slot
                Reference upRef = *(Reference *)up.Value;
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
        // If the chain is now empty, kill the driver
        if (!Values.Find({0, 0, Port, 1, 0}, true).Value)
        {
            if (DriverArray[Port])
                delete static_cast<LEDDriver *>(DriverArray[Port]);
            DriverArray[Port] = nullptr;

            Drivers none = Drivers::None;
            Values.Set(&none, sizeof(Drivers), Types::PortDriver, {0, 0, Port, 1});
        }
        else
        {
            // Re-calculate offsets for remaining segments
            DriverLED(Port);
        }
        return true;
    }
    return false;
}

bool BoardClass::ConnectI2C(BaseClass *Object, PortNumber SDA, PortNumber SCL)
{
    if (Object == nullptr || SDA > 10 || SCL > 10 || Object->Type != ObjectTypes::I2C)
        return false;

    Reference ID = Objects.GetReference(Object);
    if (!ID.IsValid())
        return false;

    // Store device reference in the SDA registry {0, 0, SDA, 1, y}
    uint8_t y = 0;
    while (y < 32)
    {
        SearchResult res = Values.Find({0, 0, SDA, 1, y}, true);
        if (!res.Value)
            break;
        if (*(Reference *)res.Value == ID)
            return true; // Already connected
        y++;
    }

    Values.Set(&ID, sizeof(Reference), Types::Reference, {0, 0, SDA, 1, y});

    // Link the SCL/SDA ports so we know which pins belong to which bus
    Values.Set(&SCL, sizeof(PortNumber), Types::PortNumber, {0, 0, SDA, 2});
    Values.Set(&SDA, sizeof(PortNumber), Types::PortNumber, {0, 0, SCL, 2});

    DriverI2C(SDA, SCL);
    return true;
}

void BoardClass::DriverI2C(PortNumber SDA, PortNumber SCL)
{
    // 1. Initialize Bus if dormant
    if (DriverArray[SDA] == nullptr)
    {
        SearchResult sdaP = Values.Find({0, 0, SDA, 0}, true);
        SearchResult sclP = Values.Find({0, 0, SCL, 0}, true);

        if (sdaP.Value && sclP.Value)
        {
            ::I2C *Bus = new ::I2C();
            if (Bus->Begin(*(::Pin *)sclP.Value, *(::Pin *)sdaP.Value, 400000))
            {
                DriverArray[SDA] = Bus;
                DriverArray[SCL] = Bus;

                Drivers dSDA = Drivers::I2C_SDA;
                Drivers dSCL = Drivers::I2C_SCL;
                Values.Set(&dSDA, sizeof(Drivers), Types::PortDriver, {0, 0, SDA, 1});
                Values.Set(&dSCL, sizeof(Drivers), Types::PortDriver, {0, 0, SCL, 1});
            }
            else
            {
                delete Bus;
                return;
            }
        }
    }

    // 2. Assign the Bus pointer to all devices sharing this SDA line
    ::I2C *ActiveBus = static_cast<::I2C *>(DriverArray[SDA]);
    uint8_t y = 0;
    while (true)
    {
        SearchResult devRef = Values.Find({0, 0, SDA, 1, y++}, true);
        if (!devRef.Value)
            break;

        I2CDeviceClass *Dev = static_cast<I2CDeviceClass *>(Objects.At(*(Reference *)devRef.Value));
        if (Dev)
            Dev->I2CDriver = ActiveBus;
    }
}

bool BoardClass::DisconnectI2C(BaseClass *Object, PortNumber SDA, PortNumber SCL)
{
    if (!Object || SDA > 10 || SCL > 10)
        return false;
    Reference ID = Objects.GetReference(Object);
    uint8_t y = 0;
    bool found = false;

    // 1. Find and Remove from SDA registry
    while (y < 32)
    {
        SearchResult res = Values.Find({0, 0, SDA, 1, y}, true);
        if (!res.Value)
            break;

        if (*(Reference *)res.Value == ID)
        {
            // Nullify the driver pointer inside the object
            static_cast<I2CDeviceClass *>(Object)->I2CDriver = nullptr;
            Values.Delete({0, 0, SDA, 1, y});
            found = true;

            // 2. Shift SDA registry to maintain a contiguous list
            uint8_t next = y + 1;
            while (true)
            {
                SearchResult up = Values.Find({0, 0, SDA, 1, next}, true);
                if (!up.Value)
                    break;

                Reference upRef = *(Reference *)up.Value;
                Values.Set(&upRef, sizeof(Reference), Types::Reference, {0, 0, SDA, 1, (uint8_t)(next - 1)});
                Values.Delete({0, 0, SDA, 1, next});
                next++;
            }
            break;
        }
        y++;
    }

    // 3. Resource Cleanup: If the SDA registry is now empty, kill the bus driver
    if (found)
    {
        SearchResult checkEmpty = Values.Find({0, 0, SDA, 1, 0}, true);
        if (!checkEmpty.Value)
        {
            if (DriverArray[SDA])
            {
                // Note: SDA and SCL likely point to the same bus object
                delete static_cast<::I2C *>(DriverArray[SDA]);
            }

            DriverArray[SDA] = nullptr;
            DriverArray[SCL] = nullptr;

            Drivers none = Drivers::None;
            Values.Set(&none, sizeof(Drivers), Types::PortDriver, {0, 0, SDA, 1});
            Values.Set(&none, sizeof(Drivers), Types::PortDriver, {0, 0, SCL, 1});

            // Remove the cross-link metadata
            Values.Delete({0, 0, SDA, 2});
            Values.Delete({0, 0, SCL, 2});
        }
    }

    return found;
}