void PortClass::AddModule(BaseClass *Object, int32_t Index)
{
    if (Object == nullptr)
        return;

    if (Modules.Add(Object, Index) == false)
        return;

    if (Index == -1) // Get correct index
        Index = Modules.Length - 1;

    uint8_t *Pin = Values.At<uint8_t>(Value::Pin);
    Drivers *Driver = Values.At<Drivers>(DriverType);
    PortTypeClass *Type = Values.At<PortTypeClass>(PortType);

    if (Pin == nullptr || Driver == nullptr || Type == nullptr)
        return;

    if (*Driver == Drivers::None)
    {
        // TODO CHECK PORT TYPE
        switch (Object->Type)
        {
        case ObjectTypes::Fan:
            if (Index != 0 || *Type != Ports::TOut)
                break;
            DriverObj = new ESP32PWM();
            ((ESP32PWM *)DriverObj)->attachPin(*Pin, 25000, 8);
            *Driver = Drivers::FanPWM;
            Object->As<FanClass>()->PWM = (ESP32PWM *)DriverObj; // Input to object
            break;
        case ObjectTypes::Input:
            if (Index != 0 || (*Type != Ports::ADC && *Type != Ports::GPIO))
                break;
            pinMode(*Pin, INPUT);
            *Driver = Drivers::Input;
            Object->As<InputClass>()->Pin = *Pin; // Input to object
            break;
        case ObjectTypes::Servo:
            if (Index != 0 || *Type != Ports::GPIO)
                break;
            DriverObj = new Servo();
            ((Servo *)DriverObj)->setPeriodHertz(50);
            ((Servo *)DriverObj)->attach(*Pin, 500, 2500);
            *Driver = Drivers::Servo;
            Object->As<ServoClass>()->ServoDriver = (Servo *)DriverObj; // Input to object
            break;
        case ObjectTypes::LEDStrip:
            if (*Type != Ports::GPIO)
                break;
            DriverObj = BeginLED(*(Object->As<LEDStripClass>()->Values.At<int32_t>(LEDStripClass::Length)), *Pin);
            *Driver = Drivers::LED;
            Object->As<LEDStripClass>()->LED = (CRGB *)DriverObj; // Input to object
            break;
        case ObjectTypes::Display:
            if (*Type != Ports::GPIO)
                break;
            DriverObj = BeginLED(*(Object->As<DisplayClass>()->Values.At<int32_t>(DisplayClass::Length)), *Pin);
            *Driver = Drivers::LED;
            Object->As<DisplayClass>()->LED = (CRGB *)DriverObj; // Input to object
            break;
        default:
            break;
        }
    }
    else if (*Driver == Drivers::LED) // Extensible
    {
        ; // TODO
    }
    return;
};
void PortClass::RemoveModule(BaseClass *Object)
{
    if (Object == nullptr)
        return;
    uint8_t *Pin = Values.At<uint8_t>(Value::Pin);
    Drivers *Driver = Values.At<Drivers>(DriverType);

    switch (*Driver)
    {
    case Drivers::FanPWM: // If one capacity, stop driver
        if (Object != Modules.At(0))
            break;
        ((ESP32PWM *)DriverObj)->detachPin(*Pin);
        delete (ESP32PWM *)DriverObj;
        Object->As<FanClass>()->PWM = nullptr; // Remove from obj
        break;
    case Drivers::Input:
        if (Object != Modules.At(0))
            break;
        Object->As<InputClass>()->Pin = -1; // Remove from obj
        break;
    case Drivers::Servo:
        if (Object != Modules.At(0))
            break;
        Object->As<ServoClass>()->ServoDriver = nullptr; // Remove from obj
        ((Servo *)DriverObj)->detach();
        delete (Servo *)DriverObj;
        break;
    case Drivers::LED: // TODO: if multiple objects edit driver
        if (Object->Type == ObjectTypes::LEDStrip)
            Object->As<LEDStripClass>()->LED = nullptr; // Remove from obj
        else if (Object->Type == ObjectTypes::Display)
            Object->As<DisplayClass>()->LED = nullptr;
        EndLED((CRGB *)DriverObj);
        break;
    default:
        break;
    }

    Modules.Remove(Object);
};

void I2CClass::AddModule(BaseClass *Object, int32_t Index)
{
    if (Object == nullptr)
        return;

    if (Modules.Add(Object, Index) == false)
        return;

    if (Index == -1) // Get correct index
        Index = Modules.Length - 1;

    if (I2C == nullptr && Index <= 1) // Start driver
    {
        if (Modules.IsValid(0, ObjectTypes::Port) == false || Modules.IsValid(1, ObjectTypes::Port) == false)
            return;

        PortClass *SDAPort = Modules.Get<PortClass>(SDA);
        PortClass *SCLPort = Modules.Get<PortClass>(SCL);

        if (SDAPort == nullptr || SCLPort == nullptr)
            return;

        if (!SDAPort->Values.IsValid(PortClass::PortType, Types::PortType) || !SCLPort->Values.IsValid(PortClass::PortType, Types::PortType) ||
            *SDAPort->Values.At<PortTypeClass>(PortClass::PortType) != Ports::I2C_SDA || *SCLPort->Values.At<PortTypeClass>(PortClass::PortType) != Ports::I2C_SCL)
            return;

        if (!SDAPort->Values.IsValid(PortClass::DriverType, Types::PortDriver) || !SCLPort->Values.IsValid(PortClass::DriverType, Types::PortDriver) ||
            *SDAPort->Values.At<Drivers>(PortClass::DriverType) != Drivers::None || *SCLPort->Values.At<Drivers>(PortClass::DriverType) != Drivers::None)
            return;

        if (!SDAPort->Values.IsValid(PortClass::Pin, Types::Byte) || !SDAPort->Values.IsValid(PortClass::Pin, Types::Byte))
            return;

        Wire.begin(*SDAPort->Values.At<uint8_t>(PortClass::Pin), *SCLPort->Values.At<uint8_t>(PortClass::Pin), 100000);
        I2C = &Wire;
        SDAPort->ValueSet(Drivers::I2C, PortClass::DriverType);
        SCLPort->ValueSet(Drivers::I2C, PortClass::DriverType);

        for (int32_t Index = Modules.FirstValid(ObjectTypes::Undefined, 2); Index < Modules.Length; Modules.Iterate(&Index))
            StartModule(Modules[Index]);
    }
    else if (I2C != nullptr)
        StartModule(Object);

    return;
};

void I2CClass::StartModule(BaseClass *Object)
{
    switch (Object->Type)
    {
    case ObjectTypes::AccGyr:
        Object->As<GyrAccClass>()->I2C = I2C;
        break;
    default:
        break;
    }
};

void I2CClass::RemoveModule(BaseClass *Object)
{
    if (Object == nullptr)
        return;

    if (Object->Type == ObjectTypes::Port && I2C != nullptr && (Modules[0] == Object || Modules[1] == Object))
    {
        I2C->end();
        I2C = nullptr;
        Modules.Get<PortClass>(SDA)->ValueSet(Drivers::None, PortClass::DriverType);
        Modules.Get<PortClass>(SCL)->ValueSet(Drivers::None, PortClass::DriverType);
        
        for (int32_t Index = Modules.FirstValid(ObjectTypes::Undefined, 2); Index < Modules.Length; Modules.Iterate(&Index))
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
        Object->As<GyrAccClass>()->I2C = nullptr;
        break;
    default:
        break;
    }
};