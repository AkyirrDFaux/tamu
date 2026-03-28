void BaseClass::Destroy()
{
    switch (Type)
    {
    case ObjectTypes::Board:
        delete (BoardClass *)this;
        break;
    case ObjectTypes::Display:
        delete (DisplayClass *)this;
        break;
    case ObjectTypes::I2C:
        delete (DisplayClass *)this;
        break;
    case ObjectTypes::Input:
        delete (InputClass *)this;
        break;
    case ObjectTypes::Sensor:
        delete (SensorClass *)this;
        break;
    case ObjectTypes::Output:
        delete (OutputClass *)this;
        break;
    case ObjectTypes::Program:
        delete (Program *)this;
        break;
    /*case ObjectTypes::LEDStrip:
        delete (LEDStripClass *)this;
        break;
    case ObjectTypes::UART:
        delete (UARTClass *)this;
        break;
    case ObjectTypes::SPI:
        // delete (SPIClass *)this;
        break;*/
    default:
        delete this;
        break;
    }
};

void BaseClass::Save() {
    /*
    ByteArray Data = ByteArray(*this).SubArray(1); // Remove Type of ID
    if (Data.Length % FLASH_PADDING != 0)
    {
        uint32_t NewLength = Data.Length + FLASH_PADDING - (Data.Length % FLASH_PADDING);
        char *NewArray = new char[NewLength];
        memcpy(NewArray, Data.Array, Data.Length);
        delete[] Data.Array;
        for (uint32_t Pad = Data.Length; Pad < NewLength; Pad++)
            NewArray[Pad] = 0;
        Data.Array = NewArray;
        Data.Length = NewLength;
    }
    HW::FlashSave(Data);
    */
};

BaseClass *CreateObject(Reference ID, ObjectTypes Type)
{
    switch (Type)
    {
    /*    case ObjectTypes::LEDStrip:
        return new LEDStripClass(ID, Flags);*/
    case ObjectTypes::Display:
        return new DisplayClass(ID);
    case ObjectTypes::I2C:
        return new I2CDeviceClass(ID);
    case ObjectTypes::Input:
        return new InputClass(ID);
    case ObjectTypes::Sensor:
        return new SensorClass(ID);
    case ObjectTypes::Output:
        return new OutputClass(ID);
    case ObjectTypes::Program:
        return new Program(ID);
    default:
        ReportError(Status::InvalidType);
        return nullptr;
    }
}

FindResult ByteArray::Find(const Reference &Location, bool StopAtReferences) const
{
    // 1. Setup the mutable search state
    const ByteArray *CurrentMap = this;
    Reference CurrentPath = Location;
    uint8_t JumpCount = 0;
    const uint8_t MaxJumps = 10; // Vital safety guard

    // THE MASTER CONTROL LOOP
    while (JumpCount < MaxJumps)
    {
        uint8_t depth = CurrentPath.PathLen();
        if (CurrentMap->Array == nullptr || CurrentMap->HeaderArray == nullptr || depth == 0)
            return {-1, Types::Undefined, nullptr};

        uint16_t Pointer = 0;
        uint16_t End = CurrentMap->HeaderAllocated;

        bool pathResolved = false;

        for (uint8_t Layer = 0; Layer < depth; Layer++)
        {
            bool layerFound = false;
            uint8_t targetIdx = CurrentPath.Path[Layer];

            for (uint16_t Search = 0; Search <= targetIdx; Search++)
            {
                if (Pointer >= End)
                    return {-1, Types::Undefined, nullptr};
                Header &header = CurrentMap->HeaderArray[Pointer];

                if (Search == targetIdx)
                {
                    // --- TERMINAL/JUNCTION LOGIC ---
                    if (header.Type == Types::Reference)
                    {
                        if (Layer < depth - 1 || !StopAtReferences)
                        {
                            // 1. Extract the Link data
                            Reference Link;
                            uint16_t copySize = (header.Length > sizeof(Reference)) ? sizeof(Reference) : header.Length;
                            memcpy(&Link, CurrentMap->Array + header.Pointer, copySize);

                            // 2. Reconstruct Path (Tail matching)
                            for (uint8_t r = Layer + 1; r < depth; r++)
                                Link = Link.Append(CurrentPath.Path[r]);

                            // 3. Prepare for next "Inception" level
                            if (Link.IsGlobal())
                            {
                                // ESP_LOGI("ByteArray", "Global Jump: %d", Link.Device);
                                BaseClass *TargetObj = Objects.At(Link);
                                if (!TargetObj)
                                    return {-1, Types::Undefined, nullptr};

                                // SWAP CONTEXT: Move to the new object's map
                                CurrentMap = &TargetObj->Values;
                            }
                            else
                            {
                                // ESP_LOGI("ByteArray", "Local Jump");
                                //  STAY IN CONTEXT: Just update path
                            }

                            CurrentPath = Link;
                            JumpCount++;
                            pathResolved = true; // Trigger restart of the While loop
                            goto break_search;
                        }
                    }

                    // Found the actual leaf
                    if (Layer == depth - 1)
                    {
                        return {(int16_t)Pointer, header.Type, CurrentMap->Array + header.Pointer};
                    }

                    if (Layer == depth - 2 && IsPacked(header.Type))
                    {
                        uint8_t subIdx = CurrentPath.Path[Layer + 1];
                        char *dataPtr = CurrentMap->Array + header.Pointer;

                        switch (header.Type)
                        {
                        case Types::Text:
                        case Types::Colour:
                            // Return as a single Byte at the offset
                            return {-2, Types::Byte, dataPtr + subIdx};

                        case Types::Vector2D:
                        case Types::Vector3D:
                        case Types::Coord2D:
                        case Types::Coord3D:
                            // Return as a Number at the float-offset
                            return {-2, Types::Number, dataPtr + (subIdx * sizeof(Number))};

                        default:
                            return {-1, Types::Undefined, nullptr};
                        }
                    }

                    // Descend
                    Pointer++;
                    End = Pointer + header.Skip;
                    layerFound = true;
                    break;
                }
                Pointer += header.Skip + 1;
            }
            if (!layerFound)
                return {-1, Types::Undefined, nullptr};
        }

    break_search:
        if (!pathResolved)
            break; // We exhausted the path or found a non-ref leaf
    }

    if (JumpCount >= MaxJumps)
        ReportError(Status::InvalidValue);
    return {-1, Types::Undefined, nullptr};
}