void PortClass::AddModule(BaseClass *Object, int32_t Index)
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
}