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

bool BoardClass::ConnectPin(BaseClass *Object, PortNumber Port)
{
    if (Object == nullptr || Port > 10) return false;
    if (Object->Type != ObjectTypes::Input && Object->Type != ObjectTypes::Sensor) return false;

    Reference ID = Objects.GetReference(Object);
    if (!ID.IsValid()) return false;

    // Path {0, 0, Port, 1, 0} is the single slot for Pin objects
    if (Values.Get<Reference>({0, 0, Port, 1, 0}).Success) return false;

    Values.Set<Reference>(ID, {0, 0, Port, 1, 0});
    DriverPin(Port);
    return true;
}

void BoardClass::DriverPin(PortNumber Port)
{
    Getter<::Pin> PortPin = Values.Get<::Pin>({0, 0, Port, 0});
    Getter<Reference> Ref = Values.Get<Reference>({0, 0, Port, 1, 0});

    if (!PortPin.Success || !Ref.Success)
    {
        Values.Set(Drivers::None, {0, 0, Port, 1});
        return;
    }

    BaseClass *Obj = Objects.At(Ref.Value);
    if (!Obj) return;

    if (Obj->Type == ObjectTypes::Input)
    {
        Values.Set(Drivers::Input, {0, 0, Port, 1});
        static_cast<InputClass *>(Obj)->InputPin = PortPin.Value;
    }
    else if (Obj->Type == ObjectTypes::Sensor)
    {
        Values.Set(Drivers::Analog, {0, 0, Port, 1});
        static_cast<SensorClass *>(Obj)->MeasPin = PortPin.Value;
    }
}

bool BoardClass::DisconnectPin(BaseClass *Object, PortNumber Port)
{
    if (Object == nullptr || Port > 10) return false;

    Reference ID = Objects.GetReference(Object);
    Reference SlotPath = {0, 0, Port, 1, 0};
    Getter<Reference> Entry = Values.Get<Reference>(SlotPath);

    if (Entry.Success && Entry.Value == ID)
    {
        if (Object->Type == ObjectTypes::Input)
            static_cast<InputClass *>(Object)->InputPin = INVALID_PIN;
        else if (Object->Type == ObjectTypes::Sensor)
            static_cast<SensorClass *>(Object)->MeasPin = INVALID_PIN;

        Values.Delete(SlotPath);
        Values.Set(Drivers::None, {0, 0, Port, 1});
        return true;
    }
    return false;
}

bool BoardClass::ConnectLED(BaseClass *Object, PortNumber Port, uint8_t Index)
{
    if (Object == nullptr || Port > 10 || Object->Type != ObjectTypes::Display)
        return false;

    Reference ID = Objects.GetReference(Object);
    if (!ID.IsValid() || Values.Get<Reference>({0, 0, Port, 1, Index}).Success)
        return false;

    Values.Set<Reference>(ID, {0, 0, Port, 1, Index});
    DriverLED(Port);
    return true;
}

void BoardClass::DriverLED(PortNumber Port)
{
    Getter<::Pin> PortPin = Values.Get<::Pin>({0, 0, Port, 0});
    if (!PortPin.Success) return;

    uint32_t TotalLength = 0;
    uint8_t y = 0;

    // 1. Calculate Total Length from refs at {0, 0, Port, 1, y}
    while (true)
    {
        Getter<Reference> Ref = Values.Get<Reference>({0, 0, Port, 1, y});
        if (!Ref.Success) break;

        BaseClass *Obj = Objects.At(Ref.Value);
        if (Obj && Obj->Type == ObjectTypes::Display)
        {
            Getter<int32_t> Len = Obj->Values.Get<int32_t>({0, 1});
            if (Len.Success) TotalLength += Len.Value;
        }
        y++;
    }

    // 2. Manage Driver and Pointers
    if (TotalLength > 0)
    {
        if (DriverArray[Port] != nullptr) delete static_cast<LEDDriver *>(DriverArray[Port]);

        LEDDriver *NewDriver = new LEDDriver(TotalLength, PortPin.Value);
        DriverArray[Port] = NewDriver;
        Values.Set(Drivers::LED, {0, 0, Port, 1});

        uint32_t CurrentOffset = 0;
        for (uint8_t i = 0; i < y; i++)
        {
            Getter<Reference> Ref = Values.Get<Reference>({0, 0, Port, 1, i});
            DisplayClass *Disp = static_cast<DisplayClass *>(Objects.At(Ref.Value));
            if (Disp)
            {
                Disp->LEDs = NewDriver->Offset(CurrentOffset);
                CurrentOffset += Disp->Values.Get<int32_t>({0, 1}).Value;
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
        Reference Path = {0, 0, Port, 1, y};
        Getter<Reference> Entry = Values.Get<Reference>(Path);
        if (!Entry.Success) break;

        if (Entry.Value == ID)
        {
            static_cast<DisplayClass *>(Object)->LEDs = nullptr;
            Values.Delete(Path);
            found = true;
            
            // Shift y-refs to maintain chain
            uint8_t Next = y + 1;
            while (true)
            {
                Getter<Reference> Up = Values.Get<Reference>({0, 0, Port, 1, Next});
                if (!Up.Success) break;
                Values.Set<Reference>(Up.Value, {0, 0, Port, 1, (uint8_t)(Next - 1)});
                Values.Delete({0, 0, Port, 1, Next});
                Next++;
            }
            break;
        }
        y++;
    }

    if (found)
    {
        if (!Values.Get<Reference>({0, 0, Port, 1, 0}).Success)
        {
            if (DriverArray[Port]) delete static_cast<LEDDriver *>(DriverArray[Port]);
            DriverArray[Port] = nullptr;
            Values.Set(Drivers::None, {0, 0, Port, 1});
        }
        else DriverLED(Port);
        return true;
    }
    return false;
}

bool BoardClass::ConnectI2C(BaseClass *Object, PortNumber Port)
{
    if (Object == nullptr || Port > 10 || Object->Type != ObjectTypes::I2C) return false;

    I2CDeviceClass *Dev = static_cast<I2CDeviceClass *>(Object);
    Reference ID = Objects.GetReference(Object);
    
    Getter<PortNumber> SdaPort = Dev->Values.Get<PortNumber>({0, 0});
    Getter<PortNumber> SclPort = Dev->Values.Get<PortNumber>({0, 1});

    if (Port != SdaPort.Value && Port != SclPort.Value) return false;

    uint8_t y = 0;
    while (y < 32)
    {
        Getter<Reference> Entry = Values.Get<Reference>({0, 0, Port, 1, y});
        if (!Entry.Success) break;
        if (Entry.Value == ID) return true; 
        y++;
    }

    Values.Set<Reference>(ID, {0, 0, Port, 1, y});
    DriverI2C(Port);
    return true;
}

void BoardClass::DriverI2C(PortNumber Port)
{
    Getter<Drivers> Role = Values.Get<Drivers>({0, 0, Port, 1});
    
    // Referral logic via Link {0, 0, Port, 2}
    if (Role.Success && Role.Value == Drivers::I2C_SCL)
    {
        Getter<PortNumber> SdaPort = Values.Get<PortNumber>({0, 0, Port, 2});
        if (SdaPort.Success) DriverI2C(SdaPort.Value);
        return;
    }

    if (DriverArray[Port] == nullptr)
    {
        Getter<Reference> FirstRef = Values.Get<Reference>({0, 0, Port, 1, 0});
        if (!FirstRef.Success) return;

        I2CDeviceClass *FirstDev = static_cast<I2CDeviceClass *>(Objects.At(FirstRef.Value));
        PortNumber SclPortNum = FirstDev->Values.Get<PortNumber>({0, 1}).Value;

        Getter<::Pin> SdaPin = Values.Get<::Pin>({0, 0, Port, 0});
        Getter<::Pin> SclPin = Values.Get<::Pin>({0, 0, SclPortNum, 0});

        if (SdaPin.Success && SclPin.Success)
        {
            ::I2C *Bus = new ::I2C();
            if (Bus->Begin(SclPin.Value, SdaPin.Value, 400000))
            {
                DriverArray[Port] = Bus;
                DriverArray[SclPortNum] = Bus;
                Values.Set(Drivers::I2C_SDA, {0, 0, Port, 1});
                Values.Set(Drivers::I2C_SCL, {0, 0, SclPortNum, 1});
                Values.Set(SclPortNum, {0, 0, Port, 2}); // Set Link
                Values.Set(Port, {0, 0, SclPortNum, 2}); // Set Link
            } else { delete Bus; return; }
        }
    }

    ::I2C *ActiveBus = static_cast<::I2C *>(DriverArray[Port]);
    uint8_t y = 0;
    while (true)
    {
        Getter<Reference> Entry = Values.Get<Reference>({0, 0, Port, 1, y++});
        if (!Entry.Success) break;
        I2CDeviceClass *Dev = static_cast<I2CDeviceClass *>(Objects.At(Entry.Value));
        if (Dev) Dev->I2CDriver = ActiveBus;
    }
}

bool BoardClass::DisconnectI2C(BaseClass *Object, PortNumber Port)
{
    if (!Object || Port > 10) return false;
    Reference ID = Objects.GetReference(Object);
    uint8_t y = 0;
    bool found = false;

    while (y < 32)
    {
        Reference Path = {0, 0, Port, 1, y};
        Getter<Reference> Entry = Values.Get<Reference>(Path);
        if (!Entry.Success) break;

        if (Entry.Value == ID)
        {
            static_cast<I2CDeviceClass *>(Object)->I2CDriver = nullptr;
            Values.Delete(Path);
            found = true;
            
            uint8_t Next = y + 1;
            while (true)
            {
                Getter<Reference> Up = Values.Get<Reference>({0, 0, Port, 1, Next});
                if (!Up.Success) break;
                Values.Set(Up.Value, {0, 0, Port, 1, (uint8_t)(Next - 1)});
                Values.Delete({0, 0, Port, 1, Next});
                Next++;
            }
            break;
        }
        y++;
    }

    if (found && !Values.Get<Reference>({0, 0, Port, 1, 0}).Success)
    {
        Getter<PortNumber> Partner = Values.Get<PortNumber>({0, 0, Port, 2});
        if (DriverArray[Port]) delete static_cast<::I2C *>(DriverArray[Port]);
        
        DriverArray[Port] = nullptr;
        Values.Set(Drivers::None, {0, 0, Port, 1});
        Values.Delete({0, 0, Port, 2});

        if (Partner.Success)
        {
            DriverArray[Partner.Value] = nullptr;
            Values.Set(Drivers::None, {0, 0, Partner.Value, 1});
            Values.Delete({0, 0, Partner.Value, 2});
        }
    }
    return found;
}