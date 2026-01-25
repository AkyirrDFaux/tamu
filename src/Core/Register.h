BaseClass *RegisterClass::At(IDClass ID) const // Returns address or nullptr if invalid
{
    if (IsValid(ID) == false)
        return nullptr;

    return Object[ID.Base()];
};

bool RegisterClass::IsValid(IDClass ID, ObjectTypes Filter) const // Returns if object at index is valid
{
    if (ID.Base() == NoID || ID.Base() >= Allocated || Object[ID.Base()] == nullptr)
        return false; // Invalid ID
    if (Filter != ObjectTypes::Undefined && Object[ID.Base()]->Type != Filter)
        return false;
    return true;
};

template <class C>
C RegisterClass::ValueGet(IDClass ID) const // Assumes valid
{
    return Object[ID.Base()]->ValueGet<C>(ID.ValueIndex());
};

template <class C>
bool RegisterClass::ValueSet(C Value, IDClass ID)
{
    if (IsValid(ID) == false)
        return false;
    return Object[ID.Base()]->ValueSet(Value, ID.ValueIndex());
}

Types RegisterClass::ValueTypeAt(IDClass ID) const // Returns if object at index is valid
{
    if (IsValid(ID) == false)
        return Types::Undefined;
    return At(ID)->Values.Type(ID.ValueIndex());
};

void RegisterClass::Expand(uint32_t NewAllocated) // Expands list to new length
{
    if (NewAllocated <= Allocated)
        return;

    NewAllocated = max(NewAllocated, Allocated * 2 / 3);

    BaseClass **NewObject = new BaseClass *[NewAllocated]; // Make larger array

    for (uint32_t Index = 0; Index < Allocated; Index++) // Copy existing
        NewObject[Index] = Object[Index];
    for (uint32_t Index = Allocated; Index < NewAllocated; Index++) // Add empty to new
        NewObject[Index] = nullptr;

    if (Allocated != 0) // Replace
        delete Object;

    Object = NewObject;
    Allocated = NewAllocated;
};

void RegisterClass::Shorten()
{
    if (Registered > Allocated / 2) // Not worth it
        return;

    uint32_t NewAllocated = Allocated;
    while (IsValid(NewAllocated - 1) && NewAllocated > 0)
        NewAllocated--;

    if (NewAllocated == Allocated)
        return;

    BaseClass **NewObject = new BaseClass *[NewAllocated]; // New Array

    for (uint32_t Index = 0; Index < NewAllocated; Index++) // Copy existing
        NewObject[Index] = Object[Index];

    if (Allocated != 0) // Replace
        delete Object;

    Object = NewObject;
    Allocated = NewAllocated;
}
bool RegisterClass::Register(BaseClass *AddObject, IDClass ID) // Add, return actual ID
{
    if (ID.Base() == NoID) // Invalid, don't do anything
        return false;

    Unregister(AddObject); // Check if needs to be unregistered first

    IDClass NewID = ID.Main();
    while (IsValid(NewID))
        NewID.ID += (1<<8);

    Expand(NewID.Base() + 1);         // Make sure there is space for it, +1!
    Object[NewID.Base()] = AddObject; // Write
    AddObject->ID = NewID;
    Registered += 1;
    return NewID == ID;
};

bool RegisterClass::Unregister(IDClass ID) // Removes object
{
    At(ID)->ID = NoID;
    Object[ID.Base()] = nullptr;
    Shorten();
    Registered -= 1;
    return true;
};

bool RegisterClass::Unregister(BaseClass *RemovedObject) // Removes object
{
    if (RemovedObject == At(RemovedObject->ID)) // If ID correct, take shortcut
        return Unregister(RemovedObject->ID);
    return false;
};

void RegisterClass::ContentDebug() const
{
    for (uint32_t Index = 1; Index <= Registered; Index++)
    {
        if (Object[Index] != nullptr)
            Serial.print(Object[Index]->ContentDebug());
    }
}