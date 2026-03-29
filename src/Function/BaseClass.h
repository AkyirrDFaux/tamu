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

SearchResult ByteArray::Find(const Bookmark &Parent, const Reference &RelativeLocation, bool StopAtReferences) const
{
    const ByteArray *CurrentMap = Parent.Map;
    uint16_t currentHIdx = Parent.HeaderIdx;
    Reference CurrentPath = RelativeLocation;

    uint8_t JumpCount = 0;
    const uint8_t MaxJumps = 10;

    // Track if we started with a valid bookmark or a root search
    bool startedFromBookmark = (Parent.Map != nullptr);

    while (JumpCount < MaxJumps)
    {
        // If no map is provided, default to 'this' and start at the root
        if (!CurrentMap)
        {
            CurrentMap = this;
            currentHIdx = 0;
            startedFromBookmark = false;
        }

        uint8_t depth = CurrentPath.PathLen();
        if (!CurrentMap->Array || !CurrentMap->HeaderArray || depth == 0)
            return {};

        // --- SEARCH BOUNDARY SETUP ---
        uint16_t Pointer;
        uint16_t End;

        // Logic: Use the Bookmark offset ONLY on the first iteration.
        // If we have jumped via a Reference (JumpCount > 0), we are 
        // effectively starting a new search at the root of a different map.
        if (startedFromBookmark && JumpCount == 0)
        {
            // Start at the first child of the bookmark
            Pointer = currentHIdx + 1;
            // Limit search to the parent's subtree
            End = currentHIdx + CurrentMap->HeaderArray[currentHIdx].Skip + 1;
        }
        else
        {
            // Standard Root Search
            Pointer = 0;
            End = CurrentMap->HeaderAllocated;
        }

        for (uint8_t Layer = 0; Layer < depth; Layer++)
        {
            bool layerFound = false;
            uint8_t targetIdx = CurrentPath.Path[Layer];
            uint8_t currentSibling = 0;

            // DIVE CORRECTION:
            // Layer 0 starts at the Pointer determined above.
            // Layers 1+ must move to the first child of the node found in the previous layer.
            if (Layer > 0)
                Pointer++;

            while (Pointer < End)
            {
                Header &h = CurrentMap->HeaderArray[Pointer];

                if (currentSibling == targetIdx)
                {
                    // 1. Reference Logic (The "Teleport")
                    if (h.Type == Types::Reference && (Layer < depth - 1 || !StopAtReferences))
                    {
                        Reference Link;
                        memcpy(&Link, CurrentMap->Array + h.Pointer, sizeof(Reference));
                        
                        // Append remaining path parts to the link
                        for (uint8_t r = Layer + 1; r < depth; r++)
                            Link = Link.Append(CurrentPath.Path[r]);

                        if (Link.IsGlobal())
                        {
                            BaseClass *TargetObj = Objects.At(Link);
                            if (!TargetObj) return {};
                            CurrentMap = &TargetObj->Values;
                        }
                        
                        // Update search state for the next iteration
                        CurrentPath = Link;
                        currentHIdx = 0; 
                        JumpCount++;
                        goto restart_inception;
                    }

                    // 2. Terminal Logic (Found the exact node)
                    if (Layer == depth - 1)
                    {
                        return {CurrentMap->Array + h.Pointer, h.Type, h.Length, {(ByteArray *)CurrentMap, Pointer}};
                    }

                    // 3. Packed Logic (Sub-indexing into Text, Color, or Vectors)
                    if (Layer == depth - 2 && IsPacked(h.Type))
                    {
                        uint8_t subIdx = CurrentPath.Path[Layer + 1];
                        char *baseData = CurrentMap->Array + h.Pointer;

                        if (h.Type == Types::Text || h.Type == Types::Colour)
                        {
                            if (subIdx >= h.Length) return {};
                            return {baseData + subIdx, Types::Byte, 1, {(ByteArray *)CurrentMap, Pointer}};
                        }
                        else
                        {
                            if ((subIdx + 1) * 4 > h.Length) return {};
                            return {baseData + (subIdx * 4), Types::Number, 4, {(ByteArray *)CurrentMap, Pointer}};
                        }
                    }

                    // 4. Descend Prep (Moving into children of this node)
                    End = Pointer + h.Skip + 1;
                    layerFound = true;
                    break;
                }

                // Move to next sibling by skipping current sibling's entire subtree
                Pointer += h.Skip + 1;
                currentSibling++;
            }
            if (!layerFound) return {};
        }
        break;
    restart_inception:;
    }
    return {};
}