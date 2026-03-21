void BoardClass::PortSetup(uint8_t Port)
{
    if (Port >= 11)
        return;

    // 1. Clear existing driver to prepare for fresh detection
    if (DriverArray[Port] != nullptr)
    {
        delete static_cast<LEDDriver *>(DriverArray[Port]);
        DriverArray[Port] = nullptr;
    }

    // 2. Get the physical Pin assigned to this port
    Getter<Pin> PortPin = Values.Get<Pin>({1, Port, 0});
    if (!PortPin.Success)
        return;

    // 3. Peak at the first object to determine the required Driver
    Getter<Reference> FirstRef = Values.Get<Reference>({1, Port, 1, 0});
    if (!FirstRef.Success)
    {
        Values.Set(Drivers::None, {1, Port, 1});
        return; // Port is empty
    }

    BaseClass *FirstObj = Objects.At(FirstRef.Value);
    if (!FirstObj){
        Values.Set(Drivers::None, {1, Port, 1});
        return;
    }
        

    // 4. AUTO-DETECTION LOGIC
    if (FirstObj->Type == ObjectTypes::Display)
    {
        // Detected a Display -> We need an LED Driver
        ESP_LOGI("Board", "Port %d: Auto-detected LED Chain", Port);

        uint32_t TotalLength = 0;
        uint8_t Slot = 0;

        Values.Set(Drivers::LED, {1, Port, 1});

        // Walk the chain to calculate total buffer size
        while (true)
        {
            Getter<Reference> Ref = Values.Get<Reference>({1, Port, 1, Slot});
            if (!Ref.Success)
                break;

            BaseClass *Obj = Objects.At(Ref.Value);
            if (Obj && Obj->Type == ObjectTypes::Display)
            {
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

            // Inject pointers into the displays
            uint32_t CurrentOffset = 0;
            for (uint8_t i = 0; i < Slot; i++)
            {
                Getter<Reference> Ref = Values.Get<Reference>({1, Port, 1, i});
                DisplayClass *Disp = static_cast<DisplayClass *>(Objects.At(Ref.Value));

                if (Disp)
                {
                    Disp->LEDs = NewDriver->Offset(CurrentOffset);
                    Getter<int32_t> Len = Disp->Values.Get<int32_t>({0, 1});
                    if (Len.Success)
                        CurrentOffset += Len.Value;
                }
            }
        }
    }
    else
        Values.Set(Drivers::None, {1, Port, 1});
    /*else if (FirstObj->Type == ObjectTypes::Sensor)
    {
        // Example: Auto-detect I2C or Analog based on Sensor type
        // DriverArray[Port] = new I2CDriver(...);
    }*/
}

void BoardClass::PortReset(BaseClass *Object)
{
    // 1. Resolve the Reference to a live object
    if (Object == nullptr)
        return;

    // 2. Clear hardware-specific pointers based on Object Type
    if (Object->Type == ObjectTypes::Display)
    {
        DisplayClass *Display = static_cast<DisplayClass *>(Object);
        Display->LEDs = nullptr;
        ESP_LOGI("Board", "Retracted Driver from Display Object");
    }
    // Add other types (Sensors, Motors, etc.) as needed
}

/*void PortClass::AddModule(BaseClass *Object, int32_t Index)
{
    if (Object == nullptr)
        return;

    if (Modules.Add(Object, Index) == false)
        return;

    if (Index == -1) // Get correct index
        Index = Modules.Length - 1;

    if (Values.Type(PortType) != Types::PortType || Values.Type(DriverType) != Types::PortDriver)
        return;

    Drivers Driver = ValueGet<Drivers>(DriverType);
    PortTypeClass Type = ValueGet<PortTypeClass>(PortType);

    if (Driver == Drivers::None)
    {
        switch (Object->Type)
        {
        case ObjectTypes::Fan:
            if (Index != 0 || Type != Ports::TOut)
                break;
            HW::ModePWM(PortPin);
            ValueSet(Drivers::FanPWM, Value::DriverType);
            Object->As<FanClass>()->PWMPin = PortPin; // Input to object
            break;
        case ObjectTypes::Input:
            if (Index != 0 || Type != Ports::GPIO)
                break;
            HW::ModeInputPullDown(PortPin);
            ValueSet(Drivers::Input, Value::DriverType);
            Object->As<InputClass>()->InputPin = PortPin; // Input to object
            break;
        case ObjectTypes::Sensor:
            if (Index != 0 || Type != Ports::ADC)
                break;
            HW::ModeAnalog(PortPin);
            ValueSet(Drivers::Input, Value::DriverType);
            Object->As<SensorClass>()->MeasPin = PortPin; // Input to object
            break;
        case ObjectTypes::Servo:
            if (Index != 0 || Type != Ports::GPIO)
                break;
            DriverObj = new Servo();
            ((Servo *)DriverObj)->setPeriodHertz(50);
            ((Servo *)DriverObj)->attach(PortPin, 500, 2500);
            ValueSet(Drivers::Servo, Value::DriverType);
            Object->As<ServoClass>()->ServoDriver = (Servo *)DriverObj; // Input to object
            break;
        case ObjectTypes::LEDStrip: // Only first one
            if (Type != Ports::GPIO)
                break;
            DriverObj = new LEDDriver(Object->As<LEDStripClass>()->ValueGet<int32_t>(LEDStripClass::Length), PortPin);
            ValueSet(Drivers::LED, Value::DriverType);
            Object->As<LEDStripClass>()->LEDs = ((LEDDriver *)DriverObj)->Offset(0); // Input to object
            break;
        case ObjectTypes::Display: // Only first one
            if (Type != Ports::GPIO)
                break;
            DriverObj = new LEDDriver(Object->As<DisplayClass>()->ValueGet<int32_t>(DisplayClass::Length), PortPin);
            ValueSet(Drivers::LED, Value::DriverType);
            Object->As<DisplayClass>()->LEDs = ((LEDDriver *)DriverObj)->Offset(0); // Input to object
            break;
        default:
            break;
        }
    }
    else if (Driver == Drivers::LED && (Object->Type == ObjectTypes::Display || Object->Type == ObjectTypes::LEDStrip)) // Extensible
        AssignLED(PortPin);

    return;
};
void PortClass::RemoveModule(BaseClass *Object)
{
    if (Object == nullptr)
        return;

    if (Values.Type(DriverType) != Types::PortDriver)
        return;

    Drivers Driver = ValueGet<Drivers>(DriverType);

    switch (Driver)
    {
    case Drivers::FanPWM: // If one capacity, stop driver
        if (Object != Modules.At(0))
            break;
        Object->As<FanClass>()->PWMPin = INVALID_PIN; // Remove from obj
        ValueSet(Drivers::None, Value::DriverType);
        break;
    case Drivers::Input:
        if (Object != Modules.At(0))
            break;
        if (Object->Type == ObjectTypes::LEDStrip)
            Object->As<InputClass>()->InputPin = INVALID_PIN; // Remove from obj
        else if (Object->Type == ObjectTypes::Display)
            Object->As<SensorClass>()->MeasPin = INVALID_PIN;
        ValueSet(Drivers::None, Value::DriverType);
        break;
    case Drivers::Servo:
        if (Object != Modules.At(0))
            break;
        Object->As<ServoClass>()->ServoDriver = nullptr; // Remove from obj
        ((Servo *)DriverObj)->detach();
        delete (Servo *)DriverObj;
        ValueSet(Drivers::None, Value::DriverType);
        break;
    case Drivers::LED:
        if (Object->Type == ObjectTypes::LEDStrip)
            Object->As<LEDStripClass>()->LEDs = LEDDriver(); // Remove from obj
        else if (Object->Type == ObjectTypes::Display)
            Object->As<DisplayClass>()->LEDs = LEDDriver();

        Modules.Remove(Object);
        AssignLED(PortPin);
        return;
    default:
        break;
    }

    Modules.Remove(Object);
};

int32_t PortClass::CountLED()
{
    int32_t LEDLength = 0;
    for (uint32_t Index = 0; Index < Modules.Length; Index++)
    {
        if (Modules.IsValid(Index) == false)
            continue;

        switch (Modules[Index]->Type)
        {
        case ObjectTypes::LEDStrip:
            if (Modules[Index]->As<LEDStripClass>()->Values.Type(LEDStripClass::Length) != Types::Integer)
                LEDLength += Modules[Index]->As<LEDStripClass>()->ValueGet<int32_t>(LEDStripClass::Length);
            break;
        case ObjectTypes::Display:
            if (Modules[Index]->As<DisplayClass>()->Values.Type(DisplayClass::Length) != Types::Integer)
                LEDLength += Modules[Index]->As<DisplayClass>()->ValueGet<int32_t>(DisplayClass::Length);
            break;
        default:
            break;
        }
    };
    return LEDLength;
};
void PortClass::AssignLED(Pin Pin)
{
    int32_t LEDLength = CountLED();
    int32_t AssignedLength = 0;
    LEDDriver *Driver = (LEDDriver *)DriverObj;

    Driver->Stop();

    if (LEDLength <= 0)
    {
        delete Driver;
        return;
    }

    *Driver = LEDDriver(LEDLength, Pin); // Start new

    for (uint32_t Index = 0; Index < Modules.Length; Index++)
    {
        if (Modules.IsValid(Index) == false)
            continue;

        switch (Modules[Index]->Type)
        {
        case ObjectTypes::LEDStrip:
            if (Modules[Index]->As<LEDStripClass>()->Values.Type(LEDStripClass::Length) != Types::Integer)
                continue;

            Modules[Index]->As<LEDStripClass>()->LEDs = Driver->Offset(AssignedLength); // Input to object
            AssignedLength += Modules[Index]->As<LEDStripClass>()->ValueGet<int32_t>(LEDStripClass::Length);
            break;
        case ObjectTypes::Display:
            if (Modules[Index]->As<DisplayClass>()->Values.Type(DisplayClass::Length) != Types::Integer)
                continue;

            Modules[Index]->As<DisplayClass>()->LEDs = Driver->Offset(AssignedLength); // Input to object
            AssignedLength += Modules[Index]->As<DisplayClass>()->ValueGet<int32_t>(DisplayClass::Length);
            break;
        default:
            break;
        }
    };
};

void I2CClass::AddModule(BaseClass *Object, int32_t Index)
{
    if (Object == nullptr)
        return;

    if (Modules.Add(Object, Index) == false)
        return;

    if (Index == -1) // Get correct index
        Index = Modules.Length - 1;

    if (I2CDriver == nullptr && Index <= 1) // Start driver
    {
        if (Modules.IsValid(0, ObjectTypes::Port) == false || Modules.IsValid(1, ObjectTypes::Port) == false)
            return;

        PortClass *SDAPort = Modules.Get<PortClass>(SDA);
        PortClass *SCLPort = Modules.Get<PortClass>(SCL);

        if (SDAPort == nullptr || SCLPort == nullptr)
            return;

        if (SDAPort->Values.Type(PortClass::PortType) != Types::PortType || SCLPort->Values.Type(PortClass::PortType) != Types::PortType ||
            SDAPort->ValueGet<PortTypeClass>(PortClass::PortType) != Ports::I2C_SDA || SCLPort->ValueGet<PortTypeClass>(PortClass::PortType) != Ports::I2C_SCL)
            return;

        if (SDAPort->Values.Type(PortClass::DriverType) != Types::PortDriver || SCLPort->Values.Type(PortClass::DriverType) != Types::PortDriver ||
            SDAPort->ValueGet<Drivers>(PortClass::DriverType) != Drivers::None || SCLPort->ValueGet<Drivers>(PortClass::DriverType) != Drivers::None)
            return;

        I2CDriver = new I2C();
        if(I2CDriver->Begin(SDAPort->PortPin, SCLPort->PortPin, 100000) == false)
        {
            delete I2CDriver;
            I2CDriver = nullptr;
            return;
        };
        SDAPort->ValueSet(Drivers::I2C, PortClass::DriverType);
        SCLPort->ValueSet(Drivers::I2C, PortClass::DriverType);

        for (uint32_t Index = Modules.FirstValid(ObjectTypes::Undefined, 2); Index < Modules.Length; Modules.Iterate(&Index))
            StartModule(Modules[Index]);
    }
    else if (I2CDriver != nullptr)
        StartModule(Object);

    return;
};

void I2CClass::StartModule(BaseClass *Object)
{
    switch (Object->Type)
    {
    case ObjectTypes::AccGyr:
        Object->As<GyrAccClass>()->I2CDriver = I2CDriver;
        break;
    default:
        break;
    }
};

void I2CClass::RemoveModule(BaseClass *Object)
{
    if (Object == nullptr)
        return;

    if (Object->Type == ObjectTypes::Port && I2CDriver != nullptr && (Modules[0] == Object || Modules[1] == Object))
    {
        delete I2CDriver;
        I2CDriver = nullptr;
        Modules.Get<PortClass>(SDA)->ValueSet(Drivers::None, PortClass::DriverType);
        Modules.Get<PortClass>(SCL)->ValueSet(Drivers::None, PortClass::DriverType);

        for (uint32_t Index = Modules.FirstValid(ObjectTypes::Undefined, 2); Index < Modules.Length; Modules.Iterate(&Index))
            StopModule(Modules[Index]);
    }
    else
        StopModule(Object);

    Modules.Remove(Object);
};

void I2CClass::StopModule(BaseClass *Object)
{
    switch (Object->Type)
    {
    case ObjectTypes::AccGyr:
        Object->As<GyrAccClass>()->I2CDriver = nullptr;
        break;
    default:
        break;
    }
};

void UpdateLED()
{
    // Loop through all ports and if they're led, call show()
    for (uint32_t Index = Board.Modules.FirstValid(ObjectTypes::Port); Index < Board.Modules.Length; Board.Modules.Iterate(&Index, ObjectTypes::Port))
    {
        PortClass *Port = Board.Modules.Get<PortClass>(Index);
        if (Port->ValueGet<Drivers>(PortClass::DriverType) == Drivers::LED)
            ((LEDDriver *)Port->DriverObj)->Show();
    }
}*/