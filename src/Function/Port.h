void BoardClass::Setup(uint16_t Index)
{
#if defined BOARD_Tamu_v2_0
    // Initialize onboard devices on root setup
    if (Index == 0)
    {
        // Reference(0,2,0) points to the local Gyro/Acc object
        I2CDeviceClass *GyroAcc = new I2CDeviceClass(Reference::Global(0, 2, 0), Flags::Auto | Flags::RunLoop);

        PortNumber sda = 8, scl = 9;
        GyroAcc->Values.Set(&sda, sizeof(PortNumber), Types::PortNumber, 1, true, true);
        GyroAcc->Values.Set(&scl, sizeof(PortNumber), Types::PortNumber, 2, true, true);

        I2CDevices model = I2CDevices::LSM6DS3TRC;
        GyroAcc->Values.Set(&model, sizeof(I2CDevices), Types::I2CDevice, 0, true, true);
        GyroAcc->Flags -= Flags::Dirty;
        InputClass *Button = new InputClass(Reference::Global(0, 2, 1), Flags::Auto | Flags::RunLoop);
        PortNumber btnPin = 10;
        Button->Values.Set(&btnPin, sizeof(PortNumber), Types::PortNumber, 1, true, true);

        Inputs inType = Inputs::ButtonWithLED;
        Button->Values.Set(&inType, sizeof(Inputs), Types::Input, 0, true, true);
        Button->Flags -= Flags::Dirty;
    }
#endif
}

PortMap BoardClass::GetPortMap(PortNumber Port)
{
    PortMap Map = {0xFFFF, 0xFFFF, 0xFFFF};

    // Navigate to Ports branch: {0, 0}
    uint16_t idx = Values.Child(Values.Child(0));

    // Jump to specific Port sibling
    for (PortNumber i = 0; i < Port && idx != 0xFFFF; i.Value++)
    {
        idx = Values.Next(idx);
    }

    if (idx != 0xFFFF)
    {
        Map.Port = idx;
        // Port Child is Pin {0}, Pin Sibling is Driver {1}
        Map.Driver = Values.Next(Values.Child(idx));
        // Driver Child is the Reference {0}
        Map.Ref = Values.Child(Map.Driver);
    }
    return Map;
}

// --- PIN CONNECTIONS ---

void SyncObjectPin(BaseClass *Obj, ::Pin P)
{
    if (Obj->Type == ObjectTypes::Input)
        static_cast<InputClass *>(Obj)->InputPin = P;
    else if (Obj->Type == ObjectTypes::Sensor)
        static_cast<SensorClass *>(Obj)->MeasPin = P;
    else if (Obj->Type == ObjectTypes::Output)
        static_cast<OutputClass *>(Obj)->PWMPin = P;
}

bool BoardClass::ConnectPin(BaseClass *Object, PortNumber Port)
{
    // Use the core Reference path logic to handle everything.
    // It's already in flash; calling it once is cheaper than local logic.
    if (!Object || Port > 10 || Values.Find({0, 0, Port, 1, 0}, true).Index != INVALID_HEADER)
        return false;

    Reference ID = Objects.GetReference(Object);
    Values.Set(&ID, sizeof(Reference), Types::Reference, {0, 0, Port, 1, 0}, true);

    DriverPin(Port);
    return true;
}

void BoardClass::DriverPin(PortNumber Port)
{
    PortMap Map = GetPortMap(Port);
    if (Map.Port == INVALID_HEADER)
        return;

    Result pinRes = Values.Get(Values.Child(Map.Port));
    Result refRes = Values.Get(Map.Ref);

    Drivers role = Drivers::None;
    ::Pin portPin = (pinRes.Value) ? *(::Pin *)pinRes.Value : INVALID_PIN;

    if (refRes.Value && HW::IsValidPin(portPin))
    {
        BaseClass *Obj = Objects.At(*(Reference *)refRes.Value);
        if (Obj)
        {
            SyncObjectPin(Obj, portPin);
            // Derive role from Type to potentially avoid a switch-case
            if (Obj->Type == ObjectTypes::Input)
                role = Drivers::Input;
            else if (Obj->Type == ObjectTypes::Sensor)
                role = Drivers::Analog;
            else if (Obj->Type == ObjectTypes::Output)
                role = Drivers::Output;
        }
    }

    Values.Set(&role, sizeof(Drivers), Types::PortDriver, Map.Driver, true);
}

bool BoardClass::DisconnectPin(BaseClass *Object, PortNumber Port)
{
    PortMap Map = GetPortMap(Port);
    // Combined null check and reference comparison
    if (Map.Ref != INVALID_HEADER)
    {
        Result res = Values.Get(Map.Ref);
        if (res.Value && *(Reference *)res.Value == Objects.GetReference(Object))
        {
            SyncObjectPin(Object, INVALID_PIN);
            Values.Delete(Map.Ref);
            DriverPin(Port);
            return true;
        }
    }
    return false;
}

// --- LED / DISPLAY CONNECTIONS ---

bool BoardClass::ConnectLED(BaseClass *Object, PortNumber Port, uint8_t Index)
{
    // Combined guard clause saves jump instructions
    if (!Object || Port > 10 || Object->Type != ObjectTypes::Display)
        return false;

    Reference ID = Objects.GetReference(Object);
    if (!ID.IsValid())
        return false;

    // Direct path-based check
    // We use the Reference overload because it's already compiled in ValueTree
    if (Values.Find({0, 0, Port, 1, Index}, true).Index != INVALID_HEADER)
        return false;

    // Single call to handle insertion/setting
    Values.Set(&ID, sizeof(Reference), Types::Reference, {0, 0, Port, 1, Index}, true);

    DriverLED(Port);
    return true;
}

void BoardClass::DriverLED(PortNumber Port)
{
    PortMap Map = GetPortMap(Port);
    if (Map.Port == INVALID_HEADER)
        return;

    // Pin is the first child of Port {0, 0, Port, 0}
    Result pinRes = Values.Get(Values.Child(Map.Port));
    if (!pinRes.Value)
        return;
    ::Pin PortPin = *(::Pin *)pinRes.Value;

    uint32_t TotalLength = 0;

    // Pass 1: Linear walk of the Driver's children to get total length
    uint16_t current = Map.Ref; // Map.Ref is the first child of the Driver node
    while (current != INVALID_HEADER)
    {
        BaseClass *Obj = Objects.At(*(Reference *)Values.Get(current).Value);
        // Use a single check for validity and type
        if (Obj && Obj->Type == ObjectTypes::Display)
        {
            // Display length is always at {0, 1}
            Result lenRes = Obj->Values.Get(Obj->Values.Next(Obj->Values.Child(0)));
            if (lenRes.Value)
                TotalLength += *(int32_t *)lenRes.Value;
        }
        current = Values.Next(current);
    }

    // Pass 2: Rebuild Hardware Driver
    if (DriverArray[Port])
        delete (LEDDriver *)DriverArray[Port];

    Drivers role = Drivers::None;
    if (TotalLength > 0)
    {
        role = Drivers::LED;
        LEDDriver *NewDriver = new LEDDriver(TotalLength, PortPin);
        DriverArray[Port] = NewDriver;

        // Pass 3: Distribute offsets using the same linear walk
        uint32_t CurrentOffset = 0;
        current = Map.Ref;
        while (current != INVALID_HEADER)
        {
            DisplayClass *Disp = (DisplayClass *)Objects.At(*(Reference *)Values.Get(current).Value);
            if (Disp)
            {
                Disp->LEDs = NewDriver->Offset(CurrentOffset);
                Result subLen = Disp->Values.Get(Disp->Values.Next(Disp->Values.Child(0)));
                if (subLen.Value)
                    CurrentOffset += *(int32_t *)subLen.Value;
            }
            current = Values.Next(current);
        }
    }
    else
        DriverArray[Port] = nullptr;

    // Update the Driver node role once at the end
    Values.Set(&role, sizeof(Drivers), Types::PortDriver, Map.Driver, true);
}

bool BoardClass::DisconnectLED(BaseClass *Object, PortNumber Port)
{
    PortMap Map = GetPortMap(Port);
    if (Map.Driver == INVALID_HEADER) return false;

    Reference ID = Objects.GetReference(Object);
    uint16_t current = Map.Ref; // Map.Ref is the first child of the Driver node

    // Linear walk to find the target object
    while (current != INVALID_HEADER)
    {
        Result res = Values.Get(current);
        if (res.Value && *(Reference *)res.Value == ID)
        {
            // Reset object state
            static_cast<DisplayClass *>(Object)->LEDs = nullptr;

            // Delete the node. The ValueTree handles header shifting internally.
            Values.Delete(current);

            // Check if the list is now empty (Driver has no children)
            if (Values.Child(Map.Driver) == INVALID_HEADER)
            {
                if (DriverArray[Port]) delete (LEDDriver *)DriverArray[Port];
                DriverArray[Port] = nullptr;

                Drivers none = Drivers::None;
                Values.Set(&none, sizeof(Drivers), Types::PortDriver, Map.Driver, true);
            }
            else
            {
                // Re-calculate lengths and offsets for remaining segments
                DriverLED(Port);
            }
            return true;
        }
        current = Values.Next(current);
    }

    return false;
}

// --- I2C CONNECTIONS ---

bool BoardClass::ConnectI2C(BaseClass *Object, PortNumber SDA, PortNumber SCL)
{
    if (!Object || SDA > 10 || SCL > 10 || Object->Type != ObjectTypes::I2C)
        return false;

    Reference ID = Objects.GetReference(Object);
    PortMap MapSDA = GetPortMap(SDA);
    if (MapSDA.Driver == INVALID_HEADER) return false;

    // 1. Check if already connected by walking the SDA driver list
    uint16_t current = MapSDA.Ref;
    uint16_t last = INVALID_HEADER;
    while (current != INVALID_HEADER)
    {
        if (*(Reference *)Values.Get(current).Value == ID) return true;
        last = current;
        current = Values.Next(current);
    }

    // 2. Add Object to SDA list (Index 'y' is effectively InsertNext after 'last')
    if (last == INVALID_HEADER) 
        Values.InsertChild(&ID, sizeof(Reference), Types::Reference, MapSDA.Driver, true);
    else 
        Values.InsertNext(&ID, sizeof(Reference), Types::Reference, last, true);

    // 3. Link SDA and SCL ports (Paths: {0,0,SDA,2} and {0,0,SCL,2})
    // Path {0,0,P,2} is the Sibling of Driver {0,0,P,1}
    uint16_t sdaLink = Values.Next(MapSDA.Driver);
    Values.Set(&SCL, sizeof(PortNumber), Types::PortNumber, sdaLink, true);

    PortMap MapSCL = GetPortMap(SCL);
    uint16_t sclLink = Values.Next(MapSCL.Driver);
    Values.Set(&SDA, sizeof(PortNumber), Types::PortNumber, sclLink, true);

    DriverI2C(SDA, SCL);
    return true;
}

void BoardClass::DriverI2C(PortNumber SDA, PortNumber SCL)
{
    PortMap MapSDA = GetPortMap(SDA);
    PortMap MapSCL = GetPortMap(SCL);

    // Guard: Ensure both ports are valid
    if (MapSDA.Port == INVALID_HEADER || MapSCL.Port == INVALID_HEADER)
        return;

    if (DriverArray[SDA] == nullptr)
    {
        // Resolve Pins: Child(Port) is always index 0 {0,0,P,0}
        ::Pin sdaPin = *(::Pin *)Values.Get(Values.Child(MapSDA.Port)).Value;
        ::Pin sclPin = *(::Pin *)Values.Get(Values.Child(MapSCL.Port)).Value;

        ::I2C *Bus = new ::I2C();
        // Speed 400kHz hardcoded to save a variable/stack space
        if (Bus->Begin(sclPin, sdaPin, 400000))
        {
            DriverArray[SDA] = Bus;
            DriverArray[SCL] = Bus;
            
            Drivers dSDA = Drivers::I2C_SDA;
            Drivers dSCL = Drivers::I2C_SCL;
            Values.Set(&dSDA, sizeof(Drivers), Types::PortDriver, MapSDA.Driver, true);
            Values.Set(&dSCL, sizeof(Drivers), Types::PortDriver, MapSCL.Driver, true);
        }
        else
        {
            delete Bus;
            return;
        }
    }

    // Distribute the Bus pointer to all connected objects
    ::I2C *ActiveBus = (::I2C *)DriverArray[SDA];
    uint16_t current = MapSDA.Ref; // Start of the SDA object list

    while (current != INVALID_HEADER)
    {
        I2CDeviceClass *Dev = (I2CDeviceClass *)Objects.At(*(Reference *)Values.Get(current).Value);
        if (Dev) Dev->I2CDriver = ActiveBus;
        
        current = Values.Next(current);
    }
}

bool BoardClass::DisconnectI2C(BaseClass *Object, PortNumber SDA, PortNumber SCL)
{
    if (!Object || SDA > 10 || SCL > 10)
        return false;

    PortMap MapSDA = GetPortMap(SDA);
    if (MapSDA.Driver == INVALID_HEADER) return false;

    Reference ID = Objects.GetReference(Object);
    uint16_t current = MapSDA.Ref;

    // Linear walk to find the device
    while (current != INVALID_HEADER)
    {
        Result res = Values.Get(current);
        if (res.Value && *(Reference *)res.Value == ID)
        {
            // Reset object pointer
            ((I2CDeviceClass *)Object)->I2CDriver = nullptr;

            // Delete the node; ValueTree automatically shifts siblings left
            Values.Delete(current);

            // Check if the bus is now empty
            if (Values.Child(MapSDA.Driver) == INVALID_HEADER)
            {
                if (DriverArray[SDA]) delete (::I2C *)DriverArray[SDA];
                DriverArray[SDA] = nullptr;
                DriverArray[SCL] = nullptr;

                Drivers none = Drivers::None;
                Values.Set(&none, sizeof(Drivers), Types::PortDriver, MapSDA.Driver, true);
                
                PortMap MapSCL = GetPortMap(SCL);
                Values.Set(&none, sizeof(Drivers), Types::PortDriver, MapSCL.Driver, true);

                // Delete the Bus-Link nodes ({0,0,P,2}) which are siblings of the Driver
                Values.Delete(Values.Next(MapSDA.Driver));
                Values.Delete(Values.Next(MapSCL.Driver));
            }
            return true;
        }
        current = Values.Next(current);
    }

    return false;
}