bool PortClass::CheckDriver(Drivers Driver)
{
    if (Driver != *DriverType.Data)
        return false;

    switch (Driver)
    {
    case Drivers::LED:
    case Drivers::I2C:
    case Drivers::FanPWM:
    case Drivers::Servo:
    case Drivers::None:
        break;
    default:
        return false;
    }
    return true;
};
void PortClass::StopDriver()
{
    switch (DriverType)
    {
    case Drivers::LED:
        EndLED((CRGB *)DriverObj);
        break;
    case Drivers::FanPWM:
        ((ESP32PWM *)DriverObj)->detachPin(Pin);
        delete (ESP32PWM *)DriverObj;
        break;
    case Drivers::Servo:
        ((Servo*)DriverObj)->detach();
        delete (Servo*)DriverObj;
        break;
    case Drivers::I2C:
        if (Attached.Length != 0) // There could be a second
            return;
        Wire.end();
        Modules.Get<PortClass>(2)->DriverType = Drivers::None; // Release
        Modules.Remove(2);                                     // Disconnect
        break;
    case Drivers::I2CClock:
        return; // Do not allow self-stop
    default:
        break;
    }
    DriverObj = nullptr;
    DriverType = Drivers::None;
};
Drivers PortClass::StartDriver()
{
    if (DriverType != Drivers::None)
        return DriverType;

    if (Attached.IsValid(0) && Attached[0]->Type == Types::Fan && Attached[0]->Modules[0] == this && (*Data == Ports::GPIO || *Data == Ports::TOut))
    {
        DriverObj = new ESP32PWM();
        ((ESP32PWM *)DriverObj)->attachPin(Pin, 25000, 8);
        DriverType = Drivers::FanPWM;
    }
    if (Attached.IsValid(0) && Attached[0]->Type == Types::Servo && Attached[0]->Modules[0] == this && *Data == Ports::GPIO)
    {
        DriverObj = new Servo();
        ((Servo *)DriverObj)->setPeriodHertz(50);
        ((Servo *)DriverObj)->attach(Pin, 500, 2500);
        DriverType = Drivers::Servo;
    }
    else if (Attached.IsValid(0) && Attached[0]->Type == Types::Display && Attached[0]->Modules[0] == this && *Data == Ports::GPIO) // Allow combining
    {
        DriverObj = BeginLED(*Attached[0]->As<DisplayClass>()->Modules.GetValue<int32_t>(1), Pin);
        DriverType = Drivers::LED;
    }
    else if (Attached.IsValid(0) && Attached[0]->Type == Types::LEDStrip && Attached[0]->Modules[0] == this && *Data == Ports::GPIO) // Allow combining
    {
        DriverObj = BeginLED(*Attached[0]->As<LEDStripClass>()->Modules.GetValue<int32_t>(1), Pin);
        DriverType = Drivers::LED;
    }
    else if (Attached.IsValid(0) && Attached[0]->Type == Types::AccGyr && Attached[0]->Modules[0] == this && (*Data == Ports::GPIO || *Data == Ports::SDA) &&
             Attached[0]->Modules.IsValid(1) && Attached[0]->Modules[1]->Type == Types::Port && Attached[0]->Modules[1] != this &&
             (*Attached[0]->Modules.GetValue<Ports>(1) == Ports::GPIO || *Attached[0]->Modules.GetValue<Ports>(1) == Ports::SCL)) // Allow combining
    {
        Modules.Add(Attached[0]->Modules[1]->ID, 2); // Take under control
        Modules.Get<PortClass>(2)->DriverType = Drivers::I2CClock;
        Wire.begin(Pin, Modules.Get<PortClass>(2)->Pin, 10000);
        DriverType = Drivers::I2C;
    }

    return DriverType;
};

void PortClass::Disconnect(BaseClass *ThatObject)
{
    Attached.Remove(ThatObject);
    StopDriver();
    StartDriver();
};
CRGB *PortClass::GetLED(BaseClass *ThatObject)
{
    if (Attached[0] == ThatObject && CheckDriver(Drivers::LED)) // Already there
        return (CRGB *)DriverObj;

    if (DriverType == Drivers::None && Attached[0] == nullptr && (ThatObject->Type == Types::Display || ThatObject->Type == Types::LEDStrip) &&
        ThatObject->Modules[0] == this && *Data == Ports::GPIO) // Suitable for attachment
    {
        Attached.Add(ThatObject, 0);
        if (StartDriver() == Drivers::LED)
            return (CRGB *)DriverObj;
        Attached.Remove(ThatObject);
    }
    return nullptr;
};
TwoWire *PortClass::GetI2C(BaseClass *ThatObject)
{
    if ((Attached[0] == ThatObject || Attached[1] == ThatObject) && CheckDriver(Drivers::I2C)) // Already there
        return &Wire;

    if (DriverType == Drivers::None && Attached[0] == nullptr && ThatObject->Type == Types::AccGyr &&
        ThatObject->Modules[0] == this && (*Data == Ports::GPIO || *Data == Ports::SDA)) // Suitable for attachment
    {
        Attached.Add(ThatObject, 0);
        if (StartDriver() == Drivers::I2C)
            return &Wire;
        Attached.Remove(ThatObject);
    }
    else if (DriverType == Drivers::I2C && Attached[1] == nullptr && ThatObject->Type == Types::AccGyr &&
             ThatObject->Modules[0] == this && ThatObject->Modules[1] == Modules[2]) // Another suitable for attachment
        Attached.Add(ThatObject, 1);

    return nullptr;
};
ESP32PWM *PortClass::GetPWM(BaseClass *ThatObject)
{
    if (Attached[0] == ThatObject && CheckDriver(Drivers::FanPWM)) // Already there
        return (ESP32PWM *)DriverObj;

    if (DriverType == Drivers::None && Attached[0] == nullptr && ThatObject->Type == Types::Fan &&
        ThatObject->Modules[0] == this && (*Data == Ports::GPIO || *Data == Ports::TOut)) // Suitable for attachment
    {
        Attached.Add(ThatObject, 0);
        if (StartDriver() == Drivers::FanPWM)
            return (ESP32PWM *)DriverObj;
        Attached.Remove(ThatObject);
    }

    return nullptr;
};

Servo *PortClass::GetServo(BaseClass *ThatObject)
{
    if (Attached[0] == ThatObject && CheckDriver(Drivers::Servo)) // Already there
        return (Servo *)DriverObj;

    if (DriverType == Drivers::None && Attached[0] == nullptr && ThatObject->Type == Types::Servo &&
        ThatObject->Modules[0] == this && *Data == Ports::GPIO) // Suitable for attachment
    {
        Attached.Add(ThatObject, 0);
        if (StartDriver() == Drivers::Servo)
            return (Servo *)DriverObj;
        Attached.Remove(ThatObject);
    }

    return nullptr;
};