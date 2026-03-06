int32_t RegisterClass::Search(Reference ID) const
{
    // 1. Validation: Reference must have at least 3 bytes to contain Index 1 and 2
    if (!ID.Indexing || ID.Length < 3)
        return -1;

    // 2. Construct the 16-bit key from the Reference path
    // ID[1] is the high byte, ID[2] is the low byte
    uint16_t Target = ((uint16_t)ID.Indexing[1] << 8) | ID.Indexing[2];

    // 3. Standard Binary Search
    int32_t Low = 0;
    int32_t High = (int32_t)Registered - 1;

    while (Low <= High)
    {
        int32_t Mid = Low + (High - Low) / 2; // Prevents overflow
        uint16_t CurrentIndex = Object[Mid].Index;

        if (CurrentIndex == Target)
        {
            if (Object[Mid].Object == nullptr)
                return -1;
            return Mid; // Found the index in the register
        }
        else if (CurrentIndex < Target)
            Low = Mid + 1;
        else
            High = Mid - 1;
    }

    return -1; // Not found
};

int32_t RegisterClass::Search(BaseClass *SearchObject) const // Removes object
{
    for (uint16_t Index = 0; Index < Registered; Index++)
    {
        if (Object[Index].Object == SearchObject)
            return Index;
    }
    return -1;
};

BaseClass *RegisterClass::At(Reference ID) const // Returns address or nullptr if invalid
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return nullptr;

    return Object[Index].Object;
};

bool RegisterClass::IsValid(Reference ID, ObjectTypes Filter) const // Returns if object at index is valid
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return false;
    if (Filter != ObjectTypes::Undefined && Object[Index].Object->Type != Filter)
        return false;
    return true;
};

template <class C>
C *RegisterClass::ValueGet(Reference ID) const // Assumes valid
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return nullptr;
    return Object[Index].Object->ValueGet<C>(ID);
};

template <class C>
bool RegisterClass::ValueSet(C Value, Reference ID)
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return false;

    return Object[Index].Object->ValueSet(Value, ID);
}

Types RegisterClass::ValueTypeAt(Reference ID) const // Returns if object at index is valid
{
    int32_t Index = Search(ID);
    if (Index == -1)
        return Types::Undefined;
    return Object[Index].Object->Values.Type(ID);
};

void RegisterClass::Expand(uint32_t NewAllocated) // Expands list to new length
{
    if (NewAllocated <= Allocated)
        return;

    NewAllocated = max(NewAllocated, Allocated * 3 / 2);

    RegisterEntry *NewObject = new RegisterEntry[NewAllocated]; // Make larger array

    if (Allocated != 0) // Replace
    {
        memmove(NewObject, Object, Registered * sizeof(RegisterEntry));
        delete Object;
    }

    Object = NewObject;
    Allocated = NewAllocated;
};

void RegisterClass::Shorten()
{
    if (Registered > Allocated / 2) // Not worth it
        return;

    uint32_t NewAllocated = Registered;

    if (NewAllocated == Allocated)
        return;

    RegisterEntry *NewObject = new RegisterEntry[NewAllocated]; // New Array

    for (uint32_t Index = 0; Index < NewAllocated; Index++) // Copy existing
        NewObject[Index] = Object[Index];

    if (Allocated != 0) // Replace
    {
        memmove(NewObject, Object, Registered * sizeof(RegisterEntry));
        delete Object;
    }

    Object = NewObject;
    Allocated = NewAllocated;
}
bool RegisterClass::Register(BaseClass *AddObject, Reference ID) // Add, return actual ID
{
    if (ID.Length < 3) // Invalid, don't do anything
        return false;

    Expand(Registered + 1);         // Make sure there is space for it


    memmove()

    Object[Index].Object = AddObject; // Write
    Object[Index].Index = ((uint16_t)ID.Indexing[1] << 8) | ID.Indexing[2];
    AddObject->ID = NewID;
    Registered += 1;
    return NewID == ID;
};

bool RegisterClass::Unregister(int32_t Index) // Removes object
{
    if (Index >= Registered || Index < 0)
        return false;

    memmove(Object + Index, Object + Index + 1, (Registered - Index - 1) * sizeof(RegisterEntry));
    Shorten();
    Registered -= 1;
    return true;
};