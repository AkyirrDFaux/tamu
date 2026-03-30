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

SearchResult ByteArray::This(const Bookmark &Parent) const
{
    if (!Parent.Map || Parent.HeaderIdx >= Parent.Map->HeaderAllocated)
        return {};

    Header &h = Parent.Map->HeaderArray[Parent.HeaderIdx];

    // If it's a reference, we must resolve it
    if (h.Type == Types::Reference)
    {
        Reference Link = Reference();
        // Extract the stored Reference from the data array
        memcpy(&Link, Parent.Map->Array + h.Pointer, h.Length);

        if (!Link.IsValid())
            return {};

        // If it's Global, we leverage the Find logic which knows how
        // to switch between different ByteArray instances (Objects)
        if (Link.IsGlobal())
            return Objects.Find(Link);
        else
            return Parent.Map->Find(Link);
    }

    // If it's NOT a reference, return the current data immediately
    return {
        Parent.Map->Array + h.Pointer,
        h.Type,
        h.Length,
        Parent};
}

SearchResult ByteArray::Find(const Reference &Location, bool StopAtReferences) const
{
    // 1. Setup the mutable search state
    const ByteArray *CurrentMap = this;
    Reference CurrentPath = Location;
    uint8_t JumpCount = 0;
    const uint8_t MaxJumps = 10; 

    while (JumpCount < MaxJumps)
    {
        uint8_t depth = CurrentPath.PathLen();
        if (CurrentMap->Array == nullptr || CurrentMap->HeaderArray == nullptr || depth == 0)
            return {}; // Returns null value and Undefined type

        uint16_t Pointer = 0;
        uint16_t End = CurrentMap->HeaderAllocated;
        bool pathResolved = false;

        for (uint8_t Layer = 0; Layer < depth; Layer++)
        {
            bool layerFound = false;
            uint8_t targetIdx = CurrentPath.Path[Layer];

            // Iterate siblings to find the one at targetIdx
            for (uint16_t Search = 0; Search <= targetIdx; Search++)
            {
                if (Pointer >= End) return {};
                Header &header = CurrentMap->HeaderArray[Pointer];

                if (Search == targetIdx)
                {
                    // --- REFERENCE TELEPORT ---
                    if (header.Type == Types::Reference && (Layer < depth - 1 || !StopAtReferences))
                    {
                        Reference Link;
                        uint16_t copySize = (header.Length > sizeof(Reference)) ? sizeof(Reference) : header.Length;
                        memcpy(&Link, CurrentMap->Array + header.Pointer, copySize);

                        // Reconstruct Tail
                        for (uint8_t r = Layer + 1; r < depth; r++)
                            Link = Link.Append(CurrentPath.Path[r]);

                        if (Link.IsGlobal())
                        {
                            BaseClass *TargetObj = Objects.At(Link);
                            if (!TargetObj) return {};
                            CurrentMap = &TargetObj->Values;
                        }

                        CurrentPath = Link;
                        JumpCount++;
                        pathResolved = true; 
                        goto break_search;
                    }

                    // --- TERMINAL LEAF ---
                    if (Layer == depth - 1)
                    {
                        return { 
                            CurrentMap->Array + header.Pointer, 
                            header.Type, 
                            header.Length, 
                            {(ByteArray*)CurrentMap, Pointer} 
                        };
                    }

                    // --- PACKED SUB-INDEXING (Vectors, Text, Colour) ---
                    if (Layer == depth - 2 && IsPacked(header.Type))
                    {
                        uint8_t subIdx = CurrentPath.Path[Layer + 1];
                        char *dataPtr = CurrentMap->Array + header.Pointer;

                        if (header.Type == Types::Text || header.Type == Types::Colour)
                        {
                            if (subIdx >= header.Length) return {};
                            return { dataPtr + subIdx, Types::Byte, 1, {(ByteArray*)CurrentMap, Pointer} };
                        }
                        else // Numbers (Vectors/Coords)
                        {
                            if ((subIdx + 1) * sizeof(Number) > header.Length) return {};
                            return { dataPtr + (subIdx * sizeof(Number)), Types::Number, (uint16_t)sizeof(Number), {(ByteArray*)CurrentMap, Pointer} };
                        }
                    }

                    // --- DESCEND ---
                    Pointer++; // Move to first child
                    End = Pointer + header.Skip; // Narrow boundary to this node's subtree
                    layerFound = true;
                    break;
                }
                
                // Jump over the entire sibling's subtree to reach the next sibling
                Pointer += header.Skip + 1;
            }
            if (!layerFound) return {};
        }

    break_search:
        if (!pathResolved) break;
    }

    if (JumpCount >= MaxJumps) ReportError(Status::InvalidValue);
    return {};
}