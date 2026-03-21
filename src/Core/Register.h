int32_t RegisterClass::Search(const Reference &ID) const
{
    // 1. We use the Global Address part of the Reference for the Register
    // Group is high byte, Device is low byte
    uint16_t Target = ((uint16_t)ID.Group << 8) | ID.Device;

    // 2. Binary Search
    int32_t Low = 0;
    int32_t High = (int32_t)Registered - 1;

    while (Low <= High)
    {
        int32_t Mid = Low + (High - Low) / 2;
        uint16_t CurrentIndex = Object[Mid].ID;

        if (CurrentIndex == Target)
        {
            if (Object[Mid].Object == nullptr)
                return -1;
            return Mid;
        }
        else if (CurrentIndex < Target)
            Low = Mid + 1;
        else
            High = Mid - 1;
    }
    return -1;
}

int32_t RegisterClass::Search(const BaseClass *SearchObject) const // Removes object
{
    for (uint16_t Index = 0; Index < Registered; Index++)
    {
        if (Object[Index].Object == SearchObject)
            return Index;
    }
    return -1;
};

Reference RegisterClass::GetReference(const BaseClass *SearchObject) const
{
    int32_t ArrayPos = Objects.Search(const_cast<BaseClass *>(SearchObject));

    if (ArrayPos != -1)
        return Reference(0, Objects.Object[ArrayPos].GroupID, Objects.Object[ArrayPos].DeviceID);

    return Reference(0, 0, 0);
}

BaseClass *RegisterClass::At(const Reference &ID) const // Returns address or nullptr if invalid
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return nullptr;

    return Object[Index].Object;
};

bool RegisterClass::IsValid(const Reference &ID, ObjectTypes Filter) const // Returns if object at index is valid
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return false;
    if (Filter != ObjectTypes::Undefined && Object[Index].Object->Type != Filter)
        return false;
    return true;
};

template <class C>
C *RegisterClass::ValueGet(const Reference &ID) const
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return nullptr;

    // IMPORTANT: Hand off ONLY the Path (Location) to the object
    return Object[Index].Object->ValueGet<C>(ID.Location);
}

template <class C>
bool RegisterClass::ValueSet(C Value, const Reference &ID)
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return false;

    // Hand off the Path to the internal ByteArray logic
    return Object[Index].Object->ValueSetup(Value, ID.Location);
}

Types RegisterClass::ValueTypeAt(const Reference &ID) const
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return Types::Undefined;

    // We use ID.Location (the Path) to look up the type inside the object's ByteArray
    return Object[Index].Object->Values.Type(ID.Location);
}

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
        delete[] Object; // Corrected to array delete
    }
    Object = NewObject;
    Allocated = NewAllocated;
}

void RegisterClass::Shorten()
{
    // Threshold check to avoid constant reallocations
    if (Registered > Allocated / 2)
        return;

    uint32_t NewAllocated = Registered;
    if (NewAllocated == Allocated || NewAllocated == 0)
        return;

    RegisterEntry *NewObject = new RegisterEntry[NewAllocated];

    if (Object != nullptr)
    {
        // Copy the current valid entries to the new smaller buffer
        memcpy(NewObject, Object, Registered * sizeof(RegisterEntry));
        delete[] Object; // FIXED: Must use delete[] for arrays
    }

    Object = NewObject;
    Allocated = NewAllocated;
}

bool RegisterClass::Register(BaseClass *AddObject, const Reference &ID, ObjectInfo Info)
{
    if (AddObject == nullptr)
        return false;

    // Use the Reference's ID fields
    uint16_t TargetKey = ((uint16_t)ID.Group << 8) | ID.Device;

    uint16_t InsertAt = 0;
    while (InsertAt < Registered && Object[InsertAt].ID < TargetKey)
        InsertAt++;

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

    // Shift elements left to overwrite the removed index
    if (Index < Registered - 1)
    {
        memmove(Object + Index,
                Object + Index + 1,
                (Registered - Index - 1) * sizeof(RegisterEntry));
    }

    Registered -= 1;

    // Attempt to shrink memory if we are now significantly under capacity
    Shorten();

    return true;
}