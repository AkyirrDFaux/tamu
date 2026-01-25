void PortClass::AddModule(BaseClass *Object, int32_t Index)
{
    if (Object == nullptr)
        return;

    if (Modules.Add(Object, Index) == false)
        return;

    if (Index == -1) // Get correct index
        Index = Modules.Length - 1;

    if (Values.Type(PortType) != Types::PortType || Values.Type(Pin) != Types::Byte || Values.Type(DriverType) != Types::PortDriver)
        return;

    uint8_t Pin = ValueGet<uint8_t>(Value::Pin);
    Drivers Driver = ValueGet<Drivers>(DriverType);
    PortTypeClass Type = ValueGet<PortTypeClass>(PortType);

    if (Driver == Drivers::None)
    {
        switch (Object->Type)
        {
        case ObjectTypes::Fan:
            if (Index != 0 || Type != Ports::TOut)
                break;
            DriverObj = new ESP32PWM();
            ((ESP32PWM *)DriverObj)->attachPin(Pin, 25000, 8);
            ValueSet(Drivers::FanPWM, Value::DriverType);
            Object->As<FanClass>()->PWM = (ESP32PWM *)DriverObj; // Input to object
            break;
        case ObjectTypes::Input:
            if (Index != 0 || (Type != Ports::ADC && Type != Ports::GPIO))
                break;
            pinMode(Pin, INPUT);
            ValueSet(Drivers::Input, Value::DriverType);
            Object->As<InputClass>()->Pin = Pin; // Input to object
            break;
        case ObjectTypes::Servo:
            if (Index != 0 || Type != Ports::GPIO)
                break;
            DriverObj = new Servo();
            ((Servo *)DriverObj)->setPeriodHertz(50);
            ((Servo *)DriverObj)->attach(Pin, 500, 2500);
            ValueSet(Drivers::Servo, Value::DriverType);
            Object->As<ServoClass>()->ServoDriver = (Servo *)DriverObj; // Input to object
            break;
        case ObjectTypes::LEDStrip: // Only first one
            if (Type != Ports::GPIO)
                break;
            DriverObj = BeginLED(Object->As<LEDStripClass>()->ValueGet<int32_t>(LEDStripClass::Length), Pin);
            ValueSet(Drivers::LED, Value::DriverType);
            Object->As<LEDStripClass>()->LED = (CRGB *)DriverObj; // Input to object
            break;
        case ObjectTypes::Display: // Only first one
            if (Type != Ports::GPIO)
                break;
            DriverObj = BeginLED(Object->As<DisplayClass>()->ValueGet<int32_t>(DisplayClass::Length), Pin);
            ValueSet(Drivers::LED, Value::DriverType);
            Object->As<DisplayClass>()->LED = (CRGB *)DriverObj; // Input to object
            break;
        default:
            break;
        }
    }
    else if (Driver == Drivers::LED && (Object->Type == ObjectTypes::Display || Object->Type == ObjectTypes::LEDStrip)) // Extensible
        AssignLED(Pin);

    return;
};
void PortClass::RemoveModule(BaseClass *Object)
{
    if (Object == nullptr)
        return;

    if (Values.Type(Pin) != Types::Byte || Values.Type(DriverType) != Types::PortDriver)
        return;

    uint8_t Pin = ValueGet<uint8_t>(Value::Pin);
    Drivers Driver = ValueGet<Drivers>(DriverType);

    switch (Driver)
    {
    case Drivers::FanPWM: // If one capacity, stop driver
        if (Object != Modules.At(0))
            break;
        ((ESP32PWM *)DriverObj)->detachPin(Pin);
        delete (ESP32PWM *)DriverObj;
        Object->As<FanClass>()->PWM = nullptr; // Remove from obj
        ValueSet(Drivers::None, Value::DriverType);
        break;
    case Drivers::Input:
        if (Object != Modules.At(0))
            break;
        Object->As<InputClass>()->Pin = -1; // Remove from obj
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
            Object->As<LEDStripClass>()->LED = nullptr; // Remove from obj
        else if (Object->Type == ObjectTypes::Display)
            Object->As<DisplayClass>()->LED = nullptr;

        Modules.Remove(Object);
        AssignLED(Pin);
        return;
    default:
        break;
    }

    Modules.Remove(Object);
};

int32_t PortClass::CountLED()
{
    int32_t LEDLength = 0;
    for (int32_t Index = 0; Index < Modules.Length; Index++)
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
void PortClass::AssignLED(uint8_t Pin)
{
    int32_t LEDLength = CountLED();
    int32_t AssignedLength = 0;
    EndLED((CRGB *)DriverObj);

    if (LEDLength <= 0)
        return;

    DriverObj = BeginLED(LEDLength, Pin);

    for (int32_t Index = 0; Index < Modules.Length; Index++)
    {
        if (Modules.IsValid(Index) == false)
            continue;

        switch (Modules[Index]->Type)
        {
        case ObjectTypes::LEDStrip:
            if (Modules[Index]->As<LEDStripClass>()->Values.Type(LEDStripClass::Length) != Types::Integer)
                continue;

            Modules[Index]->As<LEDStripClass>()->LED = &((CRGB *)DriverObj)[AssignedLength]; // Input to object
            AssignedLength += Modules[Index]->As<LEDStripClass>()->ValueGet<int32_t>(LEDStripClass::Length);
            break;
        case ObjectTypes::Display:
            if (Modules[Index]->As<DisplayClass>()->Values.Type(DisplayClass::Length) != Types::Integer)
                continue;

            Modules[Index]->As<DisplayClass>()->LED = &((CRGB *)DriverObj)[AssignedLength]; // Input to object
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

    if (I2C == nullptr && Index <= 1) // Start driver
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

        if (SDAPort->Values.Type(PortClass::Pin) != Types::Byte || SDAPort->Values.Type(PortClass::Pin) != Types::Byte)
            return;

        Wire.begin(SDAPort->ValueGet<uint8_t>(PortClass::Pin), SCLPort->ValueGet<uint8_t>(PortClass::Pin), 100000);
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