BaseClass::~BaseClass()
{
    HW::Invalidate(Objects.GetReference(this));
    Objects.Unregister(Objects.Search(this));
};

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

Bookmark ValueTree::This(uint16_t Index) const
{
    if (Index >= HeaderAllocated)
        return {};

    const Header &h = HeaderArray[Index];

    // If it's a reference, we must resolve it to find the real target
    if (h.Type == Types::Reference)
    {
        Reference Link;
        // Safety: ensure we don't overflow the Reference struct
        uint16_t copySize = (h.Length > sizeof(Reference)) ? sizeof(Reference) : h.Length;
        memcpy(&Link, Array + h.Pointer, copySize);

        if (!Link.IsValid())
            return {};

        // Recursive resolution: Find the target Bookmark
        if (Link.IsGlobal())
        {
            // Global search: likely switches to a different ValueTree instance
            return Objects.Find(Link);
        }
        else
        {
            // Local search within this same tree
            return Find(Link);
        }
    }

    // It's a standard data node: the target is itself
    return {(ValueTree *)this, Index};
}

Bookmark ValueTree::Find(const Reference &Location, bool StopAtReferences) const
{
    const ValueTree *CurrentMap = this;
    Reference CurrentPath = Location;
    uint8_t JumpCount = 0;
    const uint8_t MaxJumps = 10;

    while (JumpCount < MaxJumps)
    {
        uint8_t depth = CurrentPath.PathLen();
        if (CurrentMap->HeaderAllocated == 0 || depth == 0)
            return {};

        // Start at the root (0xFFFF is the virtual parent of index 0)
        uint16_t currentPointer = 0;
        bool pathJumped = false;

        for (uint8_t Layer = 0; Layer < depth; Layer++)
        {
            uint8_t targetSibling = CurrentPath.Path[Layer];

            // Use Next() to walk siblings
            for (uint8_t s = 0; s < targetSibling; s++)
            {
                currentPointer = CurrentMap->Next(currentPointer);
                if (currentPointer == INVALID_HEADER)
                    return {}; // Ran out of siblings
            }

            // We are now at the specific sibling for this layer
            const Header &h = CurrentMap->HeaderArray[currentPointer];

            // --- REFERENCE TELEPORT ---
            if (h.Type == Types::Reference && (Layer < depth - 1 || !StopAtReferences))
            {
                Reference Link;
                memcpy(&Link, CurrentMap->Array + h.Pointer,
                       (h.Length > sizeof(Reference)) ? sizeof(Reference) : h.Length);

                // Reconstruct Path: [Link Target] + [Remaining Path Layers]
                for (uint8_t r = Layer + 1; r < depth; r++)
                    Link = Link.Append(CurrentPath.Path[r]);

                if (Link.IsGlobal())
                {
                    BaseClass *Obj = Objects.At(Link);
                    if (!Obj)
                        return {};
                    CurrentMap = &Obj->Values;
                }

                CurrentPath = Link;
                JumpCount++;
                pathJumped = true;
                goto restart_search;
            }

            // --- TERMINAL REACHED ---
            if (Layer == depth - 1)
            {
                return {(ValueTree *)CurrentMap, currentPointer};
            }

            // --- DESCEND TO CHILD ---
            currentPointer = CurrentMap->Child(currentPointer);
            if (currentPointer == INVALID_HEADER)
                return {}; // No children to descend into
        }

    restart_search:
        if (!pathJumped)
            break;
    }

    return {};
}