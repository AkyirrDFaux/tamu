int32_t RegisterClass::Search(const Reference &ID) const
{
    // 1. Identify the unique Global ID from the Reference
    // We compare against the 16-bit packed Group/Device key
    uint16_t Target = ((uint16_t)ID.Group << 8) | ID.Device;

    // 2. Binary Search within the sorted registry
    int32_t Low = 0;
    int32_t High = (int32_t)Registered - 1;

    while (Low <= High)
    {
        int32_t Mid = Low + (High - Low) / 2;
        uint16_t CurrentKey = Object[Mid].ID;

        if (CurrentKey == Target)
        {
            if (Object[Mid].Object == nullptr)
                return -1;
            return Mid;
        }
        else if (CurrentKey < Target)
            Low = Mid + 1;
        else
            High = Mid - 1;
    }
    return -1;
}

int32_t RegisterClass::Search(const BaseClass *SearchObject) const
{
    for (uint16_t Index = 0; Index < Registered; Index++)
    {
        if (Object[Index].Object == SearchObject)
            return Index;
    }
    return -1;
}

Reference RegisterClass::GetReference(const BaseClass *SearchObject) const
{
    int32_t ArrayPos = Search(SearchObject);

    if (ArrayPos != -1)
    {
        // Return a Reference containing the IDs stored in the registry
        // Note: This returns a "Base" reference (no internal path)
        return Reference::Global(0, (uint8_t)(Object[ArrayPos].ID >> 8), (uint8_t)(Object[ArrayPos].ID & 0xFF));
    }

    return Reference(); // Returns invalid reference
}

BaseClass *RegisterClass::At(const Reference &ID) const
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return nullptr;

    return Object[Index].Object;
}

bool RegisterClass::IsValid(const Reference &ID, ObjectTypes Filter) const
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return false;
    
    if (Filter != ObjectTypes::Undefined && Object[Index].Object->Type != Filter)
        return false;
        
    return true;
}

// --- Value Accessors (Registry Level) ---

Bookmark RegisterClass::Find(const Reference &Location, bool StopAtReferences) const {
    // 1. Resolve which BaseClass owns this Reference
    BaseClass* target = At(Location);
    if (!target) return {};

    // 2. Hand off to the object's local search logic.
    // We pass {&target->Values, 0} as the root Bookmark.
    return target->Values.Find(Location, StopAtReferences);
}

/*void RegisterClass::ValueSetup(const void *Data, size_t Size, Types Type, const Reference &Location) {
    BaseClass* target = At(Location);
    if (target) {
        target->ValueSetup(Data, Size, Type, Location);
    }
}*/

// --- Memory Management ---

void RegisterClass::Expand(uint32_t NewAllocated)
{
    if (NewAllocated <= Allocated)
        return;
    
    uint32_t Grow = (Allocated == 0) ? NewAllocated : Allocated * 3 / 2;
    if (NewAllocated < Grow) NewAllocated = Grow;

    RegisterEntry *NewObject = new RegisterEntry[NewAllocated];
    if (Object != nullptr)
    {
        memcpy(NewObject, Object, Registered * sizeof(RegisterEntry));
        delete[] Object; 
    }
    Object = NewObject;
    Allocated = NewAllocated;
}

void RegisterClass::Shorten()
{
    if (Registered > Allocated / 2 || Registered == 0)
        return;

    uint32_t NewAllocated = Registered;
    RegisterEntry *NewObject = new RegisterEntry[NewAllocated];

    if (Object != nullptr)
    {
        memcpy(NewObject, Object, Registered * sizeof(RegisterEntry));
        delete[] Object;
    }

    Object = NewObject;
    Allocated = NewAllocated;
}

// --- Lifecycle ---

bool RegisterClass::Register(BaseClass *AddObject, const Reference &ID, RunInfo Info)
{
    if (AddObject == nullptr)
        return false;

    // Pack Group and Device for the sorted Registry Key
    uint16_t TargetKey = ((uint16_t)ID.Group << 8) | ID.Device;

    // Find insertion point to maintain sorted order for Binary Search
    uint16_t InsertAt = 0;
    while (InsertAt < Registered && Object[InsertAt].ID < TargetKey)
        InsertAt++;

    // Prevent duplicate registration of the same ID
    if (InsertAt < Registered && Object[InsertAt].ID == TargetKey)
        return false;

    Expand(Registered + 1);

    if (InsertAt < Registered)
    {
        memmove(&Object[InsertAt + 1], &Object[InsertAt], (Registered - InsertAt) * sizeof(RegisterEntry));
    }

    Object[InsertAt].Object = AddObject;
    Object[InsertAt].ID = TargetKey;
    Object[InsertAt].Info = Info;

    Registered += 1;
    return true;
}

bool RegisterClass::Unregister(int32_t Index)
{
    if (Index >= Registered || Index < 0)
        return false;

    if (Index < Registered - 1)
    {
        memmove(Object + Index,
                Object + Index + 1,
                (Registered - Index - 1) * sizeof(RegisterEntry));
    }

    Registered -= 1;
    Shorten();

    return true;
}