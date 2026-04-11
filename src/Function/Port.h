void BoardClass::Setup(uint16_t Index)
{
#if defined BOARD_Tamu_v2_0
    // Initialize onboard devices on root setup
    if (Index == 0)
    {
        // Reference(0,2,0) points to the local Gyro/Acc object
        I2CDeviceClass *GyroAcc = new I2CDeviceClass(Reference::Global(0, 2, 0), Flags::Auto | Flags::RunLoop);

        PortNumber sda = 8, scl = 9;
        GyroAcc->Values.SetExisting(&sda, sizeof(PortNumber), Types::PortNumber, 1);
        GyroAcc->Values.SetExisting(&scl, sizeof(PortNumber), Types::PortNumber, 2);

        I2CDevices model = I2CDevices::LSM6DS3TRC;
        GyroAcc->Values.SetExisting(&model, sizeof(I2CDevices), Types::I2CDevice, 0);
        GyroAcc->Flags -= Flags::Dirty;
        InputClass *Button = new InputClass(Reference::Global(0, 2, 1), Flags::Auto | Flags::RunLoop);
        PortNumber btnPin = 10;
        Button->Values.SetExisting(&btnPin, sizeof(PortNumber), Types::PortNumber, 1);

        Inputs inType = Inputs::ButtonWithLED;
        Button->Values.SetExisting(&inType, sizeof(Inputs), Types::Input, 0);
        Button->Flags -= Flags::Dirty;
    }
#endif
}

PortMap BoardClass::GetPortMap(PortNumber Port)
{
    uint16_t portsIdx = Values.Child(0);
    uint16_t currentPort = Values.Child(portsIdx);

    // Walk to the correct port sibling
    for (uint16_t i = 0; i < Port; i++)
    {
        currentPort = Values.Next(currentPort);
        if (currentPort == INVALID_HEADER)
            return {INVALID_HEADER, INVALID_HEADER, INVALID_HEADER, INVALID_HEADER};
    }

    uint16_t pinIdx = Values.Child(currentPort);
    uint16_t driverIdx = Values.Next(pinIdx);

    // Check if there is a Reference node attached to the driver
    uint16_t refIdx = Values.Child(driverIdx);

    return {currentPort, pinIdx, driverIdx, refIdx};
}

// --- PIN CONNECTIONS ---

void SyncObjectPin(BaseClass *Obj, ::Pin P)
{
    if (!Obj)
        return;

    if (Obj->Type == ObjectTypes::Input)
        static_cast<InputClass *>(Obj)->InputPin = P;
    else if (Obj->Type == ObjectTypes::Sensor)
        static_cast<SensorClass *>(Obj)->MeasPin = P;
    else if (Obj->Type == ObjectTypes::Output)
        static_cast<OutputClass *>(Obj)->PWMPin = P;
}

bool BoardClass::ConnectPin(BaseClass *Object, PortNumber Port)
{
    if (!Object || Port >= PORT_COUNT)
        return false;

    // 1. Get the map for this port
    PortMap map = GetPortMap(Port);
    if (map.Port == INVALID_HEADER || map.Driver == INVALID_HEADER || map.Pin == INVALID_HEADER)
        return false;

    // 2. Check if a Reference already exists at {0, 0, Port, 1, 0}
    // In our linear tree, if the node exists, it will be at map.Driver + 1
    // provided its depth is 4.
    if (map.Ref != INVALID_HEADER)
        return false;

    // 3. Create the Reference to the Object
    Reference TargetID = Objects.GetReference(Object);

    // We insert a child under the Driver node {0, 0, Port, 1}
    // Depth is 4. ReadOnly = Set.
    Values.SetChild(&TargetID, sizeof(Reference), Types::Reference, map.Driver, Tri::Set);

    // 4. Update the Hardware Driver
    DriverPin(Port);

    return true;
}

void BoardClass::DriverPin(PortNumber Port)
{
    // Always discover fresh; structural changes may have occurred
    PortMap Map = GetPortMap(Port);
    if (Map.Port == INVALID_HEADER)
        return;

    Result pinRes = Values.Get(Map.Pin);

    Drivers role = Drivers::None;
    DriverArray[Port] = nullptr;

    // Only attempt to link if the Reference node actually exists
    if (Map.Ref != INVALID_HEADER)
    {
        Result refRes = Values.Get(Map.Ref);
        if (refRes.Value && pinRes.Value)
        {
            ::Pin portPin = *(::Pin *)pinRes.Value;
            BaseClass *Obj = Objects.At(*(Reference *)refRes.Value);

            if (Obj && HW::IsValidPin(portPin))
            {
                SyncObjectPin(Obj, portPin);
                DriverArray[Port] = Obj;

                if (Obj->Type == ObjectTypes::Input)
                    role = Drivers::Input;
                else if (Obj->Type == ObjectTypes::Sensor)
                    role = Drivers::Analog;
                else if (Obj->Type == ObjectTypes::Output)
                    role = Drivers::Output;
            }
        }
    }

    // Update the Driver role node (always exists)
    Values.SetExisting(&role, sizeof(Drivers), Types::PortDriver, Map.Driver);
}

bool BoardClass::DisconnectPin(BaseClass *Object, PortNumber Port)
{
    // 1. Fresh discovery because previous deletions might have shifted indices
    PortMap Map = GetPortMap(Port);

    if (Map.Ref != INVALID_HEADER)
    {
        Result res = Values.Get(Map.Ref);
        if (res.Value && *(Reference *)res.Value == Objects.GetReference(Object))
        {
            // 2. Clear hardware links first
            SyncObjectPin(Object, INVALID_PIN);

            // 3. Delete the node (this shifts HeaderArray and DataArray)
            Values.Delete(Map.Ref);

            // 4. Update hardware state and local DriverArray
            DriverPin(Port);
            return true;
        }
    }
    return false;
}

// --- LED / DISPLAY CONNECTIONS ---

bool BoardClass::ConnectLED(BaseClass *Object, PortNumber Port, uint8_t Index)
{
    if (!Object || Port >= PORT_COUNT || Object->Type != ObjectTypes::Display)
        return false;

    Reference ID = Objects.GetReference(Object);
    if (!ID.IsValid())
        return false;

    PortMap Map = GetPortMap(Port);
    if (Map.Driver == INVALID_HEADER)
        return false;

    // 1. Capability check 
    Result typeRes = Values.Get(Map.Port);
    if (typeRes.Value && *(PortTypeClass *)typeRes.Value != Ports::GPIO)
    {
        return false;
    }

    // 2. Manual Iteration to check for Index occupancy
    // We assume the children of the Driver node are stored in order of their Index.
    uint16_t currentChild = Values.Child(Map.Driver);
    uint16_t prevChild = INVALID_HEADER;
    uint8_t count = 0;

    while (currentChild != INVALID_HEADER)
    {
        if (count == Index)
            return false; // Slot already taken
        prevChild = currentChild;
        currentChild = Values.Next(currentChild);
        count++;
    }

    // 3. Insertion via SetChild/SetNext
    // If Index is 0 and no children exist, use SetChild.
    // Otherwise, we'd need to fill gaps or append.
    // For simplicity, we append to the end of the driver's child list.
    if (prevChild == INVALID_HEADER)
    {
        Values.SetChild(&ID, sizeof(Reference), Types::Reference, Map.Driver, Tri::Set);
    }
    else
    {
        Values.SetNext(&ID, sizeof(Reference), Types::Reference, prevChild, Tri::Set);
    }

    DriverLED(Port);
    return true;
}

void BoardClass::DriverLED(PortNumber Port)
{
    PortMap Map = GetPortMap(Port);
    if (Map.Port == INVALID_HEADER) return;

    // Use PortMap to get the Pin directly
    Result pinRes = Values.Get(Map.Pin);
    if (!pinRes.Value) return;
    Pin PortPin = *(Pin *)pinRes.Value;

    uint32_t TotalLength = 0;

    // Pass 1: Sum up lengths of all attached Displays
    uint16_t current = Values.Child(Map.Driver); 
    while (current != INVALID_HEADER)
    {
        BaseClass *Obj = Objects.At(*(Reference *)Values.Get(current).Value);
        if (Obj && Obj->Type == ObjectTypes::Display)
        {
            // Jump to Display length: Child(0) is Root, Next(Root) is Length
            Result lenRes = Obj->Values.Get(Obj->Values.Next(Obj->Values.Child(0)));
            if (lenRes.Value)
                TotalLength += *(int32_t *)lenRes.Value;
        }
        current = Values.Next(current);
    }

    // Pass 2: Rebuild/Cleanup the Driver
    if (DriverArray[Port])
    {
        delete (LEDDriver *)DriverArray[Port];
        DriverArray[Port] = nullptr;
    }

    Drivers role = Drivers::None;
    if (TotalLength > 0)
    {
        role = Drivers::LED;
        LEDDriver *NewDriver = new LEDDriver(TotalLength, PortPin);
        DriverArray[Port] = NewDriver;

        // Pass 3: Distribute DMA buffer offsets to each Display
        uint32_t CurrentOffset = 0;
        current = Values.Child(Map.Driver);
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

    // Sync role to tree
    Values.SetExisting(&role, sizeof(Drivers), Types::PortDriver, Map.Driver);
}

bool BoardClass::DisconnectLED(BaseClass *Object, PortNumber Port)
{
    PortMap Map = GetPortMap(Port);
    if (Map.Driver == INVALID_HEADER)
        return false;

    Reference ID = Objects.GetReference(Object);
    
    // Use fresh child discovery to ensure we are at the head of the list
    uint16_t current = Values.Child(Map.Driver);

    while (current != INVALID_HEADER)
    {
        Result res = Values.Get(current);
        if (res.Value && *(Reference *)res.Value == ID)
        {
            // 1. Detach the hardware buffer from the specific display
            static_cast<DisplayClass *>(Object)->LEDs = nullptr;

            // 2. Remove from tree (shifts subsequent nodes)
            Values.Delete(current);

            // 3. Rebuild or Clear
            // DriverLED handles the delete/nulling of DriverArray and Role update
            // if the child list is now empty.
            DriverLED(Port);
            
            return true;
        }
        current = Values.Next(current);
    }

    return false;
}

// --- I2C CONNECTIONS ---

bool BoardClass::ConnectI2C(BaseClass *Object, PortNumber SDA, PortNumber SCL)
{
    // 1. Basic Guards
    if (!Object || SDA >= PORT_COUNT || SCL >= PORT_COUNT || Object->Type != ObjectTypes::I2C)
        return false;

    Reference ID = Objects.GetReference(Object);
    PortMap MapSDA = GetPortMap(SDA);
    PortMap MapSCL = GetPortMap(SCL);

    if (MapSDA.Driver == INVALID_HEADER || MapSCL.Driver == INVALID_HEADER)
        return false;

    // 2. Capability check: Ensure SDA supports I2C_SDA and SCL supports I2C_SCL
    // (Optional but highly recommended for CH32 hardware muxing)

    // 3. Search for existing connection on SDA
    uint16_t current = Values.Child(MapSDA.Driver);
    uint16_t last = INVALID_HEADER;
    while (current != INVALID_HEADER)
    {
        Result res = Values.Get(current);
        if (res.Value && *(Reference *)res.Value == ID)
            return true; // Already connected
        
        last = current;
        current = Values.Next(current);
    }

    // 4. Attach Object to SDA Driver list
    if (last == INVALID_HEADER)
        Values.SetChild(&ID, sizeof(Reference), Types::Reference, MapSDA.Driver, Tri::Set);
    else
        Values.SetNext(&ID, sizeof(Reference), Types::Reference, last, Tri::Set);

    // 5. Cross-Link the Ports
    // In our structure: {0,0,P,0}=Pin, {0,0,P,1}=Driver. 
    // If we want a {0,0,P,2} for the link, we append it after the Driver.
    uint16_t sdaLink = Values.Next(MapSDA.Driver);
    if (sdaLink == INVALID_HEADER) // Doesn't exist yet, create it
        Values.SetNext(&SCL, sizeof(PortNumber), Types::PortNumber, MapSDA.Driver, Tri::Set);
    else
        Values.SetExisting(&SCL, sizeof(PortNumber), Types::PortNumber, sdaLink);

    uint16_t sclLink = Values.Next(MapSCL.Driver);
    if (sclLink == INVALID_HEADER)
        Values.SetNext(&SDA, sizeof(PortNumber), Types::PortNumber, MapSCL.Driver, Tri::Set);
    else
        Values.SetExisting(&SDA, sizeof(PortNumber), Types::PortNumber, sclLink);

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
        // Use Map.Pin directly instead of re-finding child
        ::Pin sdaPin = *(::Pin *)Values.Get(MapSDA.Pin).Value;
        ::Pin sclPin = *(::Pin *)Values.Get(MapSCL.Pin).Value;

        ::I2C *Bus = new ::I2C();
        
        // 400kHz Hardcoded
        if (Bus->Begin(sclPin, sdaPin, 400000))
        {
            DriverArray[SDA] = Bus;
            DriverArray[SCL] = Bus;

            Drivers dSDA = Drivers::I2C_SDA;
            Drivers dSCL = Drivers::I2C_SCL;
            
            // Sync tree roles
            Values.SetExisting(&dSDA, sizeof(Drivers), Types::PortDriver, MapSDA.Driver);
            Values.SetExisting(&dSCL, sizeof(Drivers), Types::PortDriver, MapSCL.Driver);
        }
        else
        {
            delete Bus;
            return;
        }
    }

    // Distribute the Bus pointer to all connected objects
    ::I2C *ActiveBus = (::I2C *)DriverArray[SDA];
    
    // Safety check for the head of the reference list
    uint16_t current = Values.Child(MapSDA.Driver); 

    while (current != INVALID_HEADER)
    {
        Result res = Values.Get(current);
        if (res.Value)
        {
            I2CDeviceClass *Dev = (I2CDeviceClass *)Objects.At(*(Reference *)res.Value);
            // Verify object type before assigning driver
            if (Dev && Dev->Type == ObjectTypes::I2C)
                Dev->I2CDriver = ActiveBus;
        }
        current = Values.Next(current);
    }
}

bool BoardClass::DisconnectI2C(BaseClass *Object, PortNumber SDA, PortNumber SCL)
{
    if (!Object || SDA >= PORT_COUNT || SCL >= PORT_COUNT)
        return false;

    PortMap MapSDA = GetPortMap(SDA);
    if (MapSDA.Driver == INVALID_HEADER)
        return false;

    Reference ID = Objects.GetReference(Object);
    // Start discovery at the actual child of the driver
    uint16_t current = Values.Child(MapSDA.Driver);

    while (current != INVALID_HEADER)
    {
        Result res = Values.Get(current);
        if (res.Value && *(Reference *)res.Value == ID)
        {
            // 1. Detach Object
            ((I2CDeviceClass *)Object)->I2CDriver = nullptr;

            // 2. Remove Device from Tree
            Values.Delete(current);

            // 3. Conditional Bus Tear-down
            if (Values.Child(MapSDA.Driver) == INVALID_HEADER)
            {
                // Clean up hardware instance
                if (DriverArray[SDA])
                {
                    delete (::I2C *)DriverArray[SDA];
                    DriverArray[SDA] = nullptr;
                    DriverArray[SCL] = nullptr;
                }

                Drivers none = Drivers::None;
                Values.SetExisting(&none, sizeof(Drivers), Types::PortDriver, MapSDA.Driver);
                
                // Refresh MapSCL because the SDA Delete might have shifted indices
                PortMap MapSCL = GetPortMap(SCL);
                Values.SetExisting(&none, sizeof(Drivers), Types::PortDriver, MapSCL.Driver);

                // 4. Delete the Port Link nodes {0,0,P,2}
                // We find them by looking for the sibling immediately following the Driver node
                uint16_t sdaLink = Values.Next(MapSDA.Driver);
                if (sdaLink != INVALID_HEADER) Values.Delete(sdaLink);

                // Re-fetch SCL one last time for safety before final delete
                MapSCL = GetPortMap(SCL);
                uint16_t sclLink = Values.Next(MapSCL.Driver);
                if (sclLink != INVALID_HEADER) Values.Delete(sclLink);
            }
            return true;
        }
        current = Values.Next(current);
    }

    return false;
}