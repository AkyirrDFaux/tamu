void BoardClass::Setup(const Reference &Index)
{
#if defined BOARD_Tamu_v2_0
    // Initialize onboard devices only when Setup is called with the root path (Length 0)
    if (Index.PathLen() == 0)
    {
        // Reference(0,2,0) points to the local Gyro/Acc object
        I2CDeviceClass *GyroAcc = new I2CDeviceClass(Reference::Global(0, 2, 0), {Flags::Auto, 1});

        GyroAcc->ValueSetup<PortNumber>(8, {0, 0}); // SDA Port Index
        GyroAcc->ValueSetup<PortNumber>(9, {0, 1}); // SCL Port Index
        GyroAcc->ValueSetup(I2CDevices::LSM6DS3TRC, {0});

        InputClass *Button = new InputClass(Reference::Global(0, 2, 1), {Flags::Auto, 1});
        Button->ValueSetup<PortNumber>(10, {0,0});
        Button->ValueSetup<Inputs>(Inputs::ButtonWithLED, {0});
    }
#endif
}

void BoardClass::DriverStop(PortNumber Port)
{
    if (Port >= 11)
        return;

    // Board config still lives in Board-Branch {1}
    Getter<Drivers> CurrentRole = Values.Get<Drivers>({1, Port, 1});
    if (!CurrentRole.Success || CurrentRole.Value == Drivers::None)
        return;

    // 1. Handle I2C Redirection (SCL -> SDA)
    if (CurrentRole.Value == Drivers::I2C_SCL)
    {
        Getter<PortNumber> SdaPort = Values.Get<PortNumber>({1, Port, 2});
        if (SdaPort.Success)
        {
            Getter<Drivers> PartnerRole = Values.Get<Drivers>({1, SdaPort.Value, 1});
            if (PartnerRole.Success && PartnerRole.Value == Drivers::I2C_SDA)
            {
                DriverStop(SdaPort);
                return;
            }
        }
    }

    // 2. Handle SDA Deletion (The Bus Owner)
    else if (CurrentRole.Value == Drivers::I2C_SDA)
    {
        bool stillInUse = false;
        Getter<PortNumber> SclPort = Values.Get<PortNumber>({1, Port, 2});

        uint8_t Slot = 0;
        while (true)
        {
            Getter<Reference> Entry = Values.Get<Reference>({1, Port, 1, Slot++});
            if (!Entry.Success)
                break;

            BaseClass *Obj = Objects.At(Entry.Value);
            if (Obj && Obj->Type == ObjectTypes::I2C)
            {
                I2CDeviceClass *Sensor = static_cast<I2CDeviceClass *>(Obj);

                // I2C Hardware config is still at {0, 0} and {0, 1}
                Getter<PortNumber> SdaTarget = Sensor->Values.Get<PortNumber>({0, 0});
                Getter<PortNumber> SclTarget = Sensor->Values.Get<PortNumber>({0, 1});

                if (SdaTarget.Success && SclTarget.Success)
                {
                    if (SdaTarget.Value == Port && SclTarget.Value == SclPort.Value)
                    {
                        stillInUse = true;
                        break;
                    }
                }
            }
        }

        if (!stillInUse)
        {
            if (DriverArray[Port] != nullptr)
            {
                delete static_cast<I2C *>(DriverArray[Port]);
                ESP_LOGI("Board", "I2C Bus stopped on ports %d (SDA) and %d (SCL)", Port, SclPort.Value);
            }

            DriverArray[Port] = nullptr;
            if (SclPort.Value < 11)
                DriverArray[SclPort.Value] = nullptr;

            Values.Delete({1, Port, 2});
            Values.Delete({1, SclPort.Value, 2});
            Values.Set(Drivers::None, {1, SclPort.Value, 1});
            Values.Set(Drivers::None, {1, Port, 1});
        }
    }

    // 3. Simple Drivers (LED)
    else if (CurrentRole.Value == Drivers::LED && DriverArray[Port] != nullptr)
    {
        delete static_cast<LEDDriver *>(DriverArray[Port]);
        DriverArray[Port] = nullptr;
        Values.Set(Drivers::None, {1, Port, 1});
    }
    else if (CurrentRole.Value == Drivers::Input || CurrentRole.Value == Drivers::Analog)
    {
        Values.Set(Drivers::None, {1, Port, 1});
    }
}

void BoardClass::DriverStart(PortNumber Port)
{
    Getter<Pin> PortPin = Values.Get<Pin>({1, Port, 0});
    if (!PortPin.Success)
        return;

    Getter<Reference> FirstRef = Values.Get<Reference>({1, Port, 1, 0});
    if (!FirstRef.Success)
    {
        Values.Set(Drivers::None, {1, Port, 1});
        Values.Delete({1, Port, 2});
        return;
    }

    BaseClass *FirstObj = Objects.At(FirstRef.Value);
    if (!FirstObj)
    {
        Values.Set(Drivers::None, {1, Port, 1});
        return;
    }

    // 1. LED/Display Chain Auto-Detection
    if (FirstObj->Type == ObjectTypes::Display)
    {
        uint32_t TotalLength = 0;
        uint8_t Slot = 0;

        Values.Set(Drivers::LED, {1, Port, 1});

        while (true)
        {
            Getter<Reference> Ref = Values.Get<Reference>({1, Port, 1, Slot});
            if (!Ref.Success)
                break;

            BaseClass *Obj = Objects.At(Ref.Value);
            if (Obj && Obj->Type == ObjectTypes::Display)
            {
                // INDEX UPDATE: Display Length is now in Branch {0, 1}
                Getter<int32_t> Len = Obj->Values.Get<int32_t>({0, 1});
                if (Len.Success)
                    TotalLength += Len.Value;
            }
            Slot++;
        }

        if (TotalLength > 0)
        {
            LEDDriver *NewDriver = new LEDDriver(TotalLength, PortPin.Value);
            DriverArray[Port] = NewDriver;

            uint32_t CurrentOffset = 0;
            for (uint8_t i = 0; i < Slot; i++)
            {
                Getter<Reference> Ref = Values.Get<Reference>({1, Port, 1, i});
                DisplayClass *Disp = static_cast<DisplayClass *>(Objects.At(Ref.Value));

                if (Disp)
                {
                    Disp->LEDs = NewDriver->Offset(CurrentOffset);
                    // INDEX UPDATE: Display Length is now in Branch {0, 1}
                    Getter<int32_t> Len = Disp->Values.Get<int32_t>({0, 1});
                    if (Len.Success)
                        CurrentOffset += Len.Value;
                }
            }
        }
    }
    else if (FirstObj->Type == ObjectTypes::Input)
    {
        InputClass *Input = static_cast<InputClass *>(FirstObj);
        Values.Set(Drivers::Input, {1, Port, 1});
        Input->InputPin = PortPin.Value;
    }
    else if (FirstObj->Type == ObjectTypes::Sensor)
    {
        SensorClass *Input = static_cast<SensorClass *>(FirstObj);
        Values.Set(Drivers::Analog, {1, Port, 1});
        Input->MeasPin = PortPin.Value;
    }
    // 2. I2C Logic (Referral System)
    else if (FirstObj->Type == ObjectTypes::I2C)
    {
        I2CDeviceClass *Sensor = static_cast<I2CDeviceClass *>(FirstObj);
        // I2C configuration is at {0, 0} and {0, 1}
        Getter<PortNumber> SdaPort = Sensor->Values.Get<PortNumber>({0, 0});
        Getter<PortNumber> SclPort = Sensor->Values.Get<PortNumber>({0, 1});

        if (!SdaPort.Success || !SclPort.Success)
            return;

        if (SclPort.Value == Port)
        {
            DriverStart(SdaPort.Value);
            return;
        }
        else if (SdaPort.Value == Port)
        {
            Getter<Drivers> CurrentRole = Values.Get<Drivers>({1, Port, 1});

            if (!CurrentRole.Success || CurrentRole.Value == Drivers::None)
            {
                Getter<Pin> SdaPin = Values.Get<Pin>({1, Port, 0});
                Getter<Pin> SclPin = Values.Get<Pin>({1, SclPort.Value, 0});

                if (SdaPin.Success && SclPin.Success)
                {
                    I2C *Bus = new I2C();
                    if (Bus->Begin(SclPin.Value, SdaPin.Value, 400000))
                    {
                        Values.Set(Drivers::I2C_SDA, {1, Port, 1});
                        Values.Set(Drivers::I2C_SCL, {1, SclPort.Value, 1});
                        Values.Set(SclPort.Value, {1, Port, 2});
                        Values.Set(Port, {1, SclPort.Value, 2});

                        DriverArray[Port] = Bus;
                        DriverArray[SclPort.Value] = Bus;
                    }
                    else
                    {
                        delete Bus;
                        return;
                    }
                }
            }

            I2C *ActiveBus = static_cast<I2C *>(DriverArray[Port]);
            if (ActiveBus != nullptr)
            {
                uint8_t Slot = 0;
                while (true)
                {
                    Getter<Reference> Entry = Values.Get<Reference>({1, Port, 1, Slot++});
                    if (!Entry.Success)
                        break;

                    BaseClass *Obj = Objects.At(Entry.Value);
                    if (Obj && Obj->Type == ObjectTypes::I2C)
                    {
                        I2CDeviceClass *Dev = static_cast<I2CDeviceClass *>(Obj);
                        Getter<PortNumber> TargetScl = Dev->Values.Get<PortNumber>({0, 1});
                        if (TargetScl.Success && TargetScl.Value == SclPort.Value)
                        {
                            Dev->I2CDriver = ActiveBus;
                        }
                    }
                }
            }
        }
    }
}

void BoardClass::PortSetup(PortNumber Port)
{
    if (Port >= 11)
        return;
    DriverStop(Port);
    DriverStart(Port);
}

void BoardClass::PortReset(BaseClass *Object)
{
    if (Object == nullptr)
        return;

    switch (Object->Type)
    {
    case ObjectTypes::Display:
    {
        DisplayClass *Display = static_cast<DisplayClass *>(Object);
        Display->LEDs = nullptr;
        break;
    }
    case ObjectTypes::I2C:
    {
        I2CDeviceClass *Sensor = static_cast<I2CDeviceClass *>(Object);
        Sensor->I2CDriver = nullptr;
        break;
    }
    case ObjectTypes::Input:
    {
        static_cast<InputClass *>(Object)->InputPin = INVALID_PIN;
        break;
    }
    case ObjectTypes::Sensor:
    {
        static_cast<SensorClass *>(Object)->MeasPin = INVALID_PIN;
        break;
    }
    default:
        break;
    }
}