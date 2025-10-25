class RegisterClass
{
public:
    BaseClass **Object = nullptr;
    int32_t Allocated = 0;
    int32_t Registered = 0;

    RegisterClass();

    BaseClass *operator[](IDClass ID);
    BaseClass *Find(IDClass ID);
    bool IsValid(IDClass ID);
    void Expand(int32_t NewAllocated);
    void Shorten();

    bool Register(BaseClass *AddObject, IDClass ID = RandomID);
    bool Unregister(IDClass ID);
    bool Unregister(BaseClass *RemovedObject);

    bool Move(int32_t Index, int32_t NewIndex);
    String ContentDebug();
};

RegisterClass::RegisterClass()
{
};

BaseClass *RegisterClass::operator[](IDClass ID)
{
    return Find(ID);
};

BaseClass *RegisterClass::Find(IDClass ID) // Returns address or nullptr if invalid
{
    if (ID.Base() == NoID || ID.Base() >= Allocated)
        return nullptr; // Invalid ID

    return Object[ID.Base()];
};

bool RegisterClass::IsValid(IDClass ID) // Returns if object at index is valid
{
    return (Find(ID) != nullptr);
};

void RegisterClass::Expand(int32_t NewAllocated) // Expands list to new length
{
    if (NewAllocated <= Allocated)
        return;

    NewAllocated = max(NewAllocated, Allocated * 2 / 3);

    BaseClass **NewObject = new BaseClass *[NewAllocated]; // Make larger array

    for (int32_t Index = 0; Index < Allocated; Index++) // Copy existing
        NewObject[Index] = Object[Index];
    for (int32_t Index = Allocated; Index < NewAllocated; Index++) // Add empty to new
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

    int32_t NewAllocated = Allocated;
    while (IsValid(NewAllocated - 1) && NewAllocated > 0)
        NewAllocated--;

    if (NewAllocated == Allocated)
        return;

    BaseClass **NewObject = new BaseClass *[NewAllocated]; // New Array

    for (int32_t Index = 0; Index < NewAllocated; Index++) // Copy existing
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
    Find(ID)->ID = NoID;
    Object[ID.Base()] = nullptr;
    Shorten();
    Registered -= 1;
    return true;
};

bool RegisterClass::Unregister(BaseClass *RemovedObject) // Removes object
{
    if (RemovedObject == Find(RemovedObject->ID)) // If ID correct, take shortcut
        return Unregister(RemovedObject->ID);
    return false;
};

String RegisterClass::ContentDebug()
{
    String Text = "";

    for (int32_t Index = 1; Index <= Registered; Index++)
    {
        if (Object[Index] != nullptr && Object[Index]->ReferencesCount == 0)
            Text += Object[Index]->ContentDebug(0);
    }
    return Text;
}