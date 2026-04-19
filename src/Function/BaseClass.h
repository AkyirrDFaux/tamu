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
    case ObjectTypes::Undefined:
        return new BaseClass(&DefaultTable,ID);
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
        return nullptr;
    }
}

Bookmark ValueTree::This(uint16_t Index) const
{
    // 1. Boundary check using active HeaderLength
    if (Index >= HeaderLength)
        return { (ValueTree*)this, INVALID_HEADER };

    const Header &h = HeaderArray[Index];

    // 2. Reference Resolution Logic
    if (h.Type == Types::Reference && h.Pointer != INVALID_POINTER)
    {
        Reference Link;
        // Safety: Ensure we don't overflow our stack-allocated Reference struct
        uint16_t copySize = (h.Length > sizeof(Reference)) ? sizeof(Reference) : h.Length;
        
        // Use DataArray with the 4-byte word offset
        memcpy(&Link, DataArray + (h.Pointer * 4), copySize);

        if (!Link.IsValid())
            return { (ValueTree*)this, INVALID_HEADER };

        // 3. Target Resolution
        Bookmark target;
        if (Link.IsGlobal())
        {
            // Global search: resolving through a registry or object manager
            target = Objects.Find(Link);
        }
        else
        {
            // Local search: finding the path within this specific tree
            // Note: StopAtReferences = true prevents infinite recursion loops
            target = Find(Link, true);
        }

        // 4. Recursive Dereferencing
        // If the found target is ALSO a reference, we follow the chain.
        if (target.Index != INVALID_HEADER && target.Map != nullptr)
        {
            const Header &targetH = target.Map->HeaderArray[target.Index];
            if (targetH.Type == Types::Reference)
            {
                // Follow the rabbit hole one level deeper
                return target.Map->This(target.Index);
            }
        }

        return target;
    }

    // 5. Standard Node: The target is the node itself
    return { (ValueTree*)this, Index };
}

Bookmark ValueTree::Find(const Reference &Location, bool StopAtReferences) const
{
    const ValueTree *CurrentMap = this;
    Reference CurrentPath = Location;
    uint8_t JumpCount = 0;
    const uint8_t MaxJumps = 10; // Prevent infinite redirection loops

    while (JumpCount < MaxJumps)
    {
        uint8_t depth = CurrentPath.PathLen();
        
        // Use HeaderLength for active count; depth 0 is an invalid search
        if (CurrentMap->HeaderLength == 0 || depth == 0)
            return { (ValueTree*)CurrentMap, INVALID_HEADER };

        // Start at the first root
        uint16_t currentPointer = 0;
        bool pathJumped = false;

        for (uint8_t Layer = 0; Layer < depth; Layer++)
        {
            uint8_t targetSibling = CurrentPath.Path[Layer];

            // 1. Walk across siblings
            for (uint8_t s = 0; s < targetSibling; s++)
            {
                currentPointer = CurrentMap->Next(currentPointer);
                if (currentPointer == INVALID_HEADER)
                    return { (ValueTree*)CurrentMap, INVALID_HEADER }; 
            }

            // 2. Inspect the node at this specific sibling
            const Header &h = CurrentMap->HeaderArray[currentPointer];

            // --- REFERENCE TELEPORT ---
            // If we hit a reference mid-path, or at the end (unless StopAtReferences is true)
            if (h.Type == Types::Reference && (Layer < depth - 1 || !StopAtReferences))
            {
                Reference Link;
                // Aligned data access: DataArray + (h.Pointer * 4)
                uint16_t copySize = (h.Length > sizeof(Reference)) ? sizeof(Reference) : h.Length;
                memcpy(&Link, CurrentMap->DataArray + (h.Pointer * 4), copySize);

                // Reconstruct Path: [The Link Target] + [Everything below the current layer]
                // Example: Finding [0, 5, 2] hits a link at [0, 5]. 
                // Link is [8, 8]. New path becomes [8, 8, 2].
                for (uint8_t r = Layer + 1; r < depth; r++)
                {
                    Link = Link.Append(CurrentPath.Path[r]);
                }

                // Global vs Local Teleport
                if (Link.IsGlobal())
                {
                    BaseClass *Obj = Objects.At(Link); // External registry lookup
                    if (!Obj) return { (ValueTree*)CurrentMap, INVALID_HEADER };
                    CurrentMap = &Obj->Values;
                }
                // (If Local, CurrentMap remains the same, but CurrentPath is updated)

                CurrentPath = Link;
                JumpCount++;
                pathJumped = true;
                break; // Exit the Layer loop to restart_search via the 'while'
            }

            // --- TERMINAL REACHED ---
            if (Layer == depth - 1)
            {
                return { (ValueTree *)CurrentMap, currentPointer };
            }

            // --- DESCEND TO CHILD ---
            currentPointer = CurrentMap->Child(currentPointer);
            if (currentPointer == INVALID_HEADER)
                return { (ValueTree*)CurrentMap, INVALID_HEADER };
        }

        // If we didn't hit a teleport during the layer walk, we're done (found nothing or returned above)
        if (!pathJumped)
            break;
            
        // restart_search logic is handled by the 'while' loop and 'pathJumped' flag
    }

    return { (ValueTree*)this, INVALID_HEADER };
}