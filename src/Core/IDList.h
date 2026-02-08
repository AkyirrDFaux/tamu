inline IDList::IDList(const IDList &Copied)
{
    Length = Copied.Length;
    if (Length > 0)
    {
        IDs = new IDClass[Length];
        memcpy((void*)IDs, Copied.IDs, sizeof(IDClass) * Length);
    }
    else
    {
        IDs = nullptr;
    }
};

inline void IDList::operator=(const IDList &Copied)

{
    if (this == &Copied)
        return;

    if (IDs != nullptr)
        delete[] IDs;

    Length = Copied.Length;
    if (Length > 0)
    {
        IDs = new IDClass[Length];
        memcpy((void*)IDs, Copied.IDs, sizeof(IDClass) * Length);
    }
    else
    {
        IDs = nullptr;
    }
};

// OBJECTS

bool IDList::IsValid(int32_t Index, ObjectTypes Filter) const
{
    if ((uint32_t)Index >= Length || Index < 0)
        return false;

    return Objects.IsValid(IDs[Index], Filter);
};

ObjectTypes IDList::TypeAt(int32_t Index) const
{
    if (IsValid(Index) == false) // Invalid or reference to main obj
        return ObjectTypes::Undefined;

    return At(Index)->Type;
};

BaseClass *IDList::At(int32_t Index) const // Returns address or nullptr if invalid
{
    if (IsValid(Index) == false)
        return nullptr;

    return Objects.At(IDs[Index]);
};

int32_t IDList::FirstValid(ObjectTypes Filter, int32_t Start) const
{
    int32_t Index = Start;
    while ((uint32_t)Index <= Length && !IsValid(Index, Filter))
        Index++;
    return Index;
};

void IDList::Iterate(uint32_t *Index, ObjectTypes Filter) const
{
    (*Index)++;
    // Skip to next if: A)Is invalid; B)Filter is not undefined and Type of object isn't one filtered
    while (*Index <= Length && !IsValid(*Index, Filter))
        (*Index)++;
};

// VALUES
Types IDList::ValueTypeAt(int32_t Index) const
{
    if (IsValid(Index) == false) // Invalid or reference to main obj
        return Types::Undefined;

    return At(Index)->Values.Type(IDs[Index].ValueIndex());
};

template <class C>
C IDList::ValueGet(int32_t Index) const // Assumes valid
{
    return Objects.ValueGet<C>(IDs[Index]);
};

template <class C>
bool IDList::ValueSet(C Value, int32_t Index)
{
    if (IsValid(Index) == false)
        return false;
    return Objects.ValueSet(Value, IDs[Index]);
}

// MEMORY MANAGEMENT

void IDList::Expand(uint8_t NewLength) // Expands list to new length
{
    if (NewLength <= Length)
        return;

    IDClass *NewIDs = new IDClass[NewLength];

    for (uint32_t Index = 0; Index < Length; Index++) // Copy existing
        NewIDs[Index] = IDs[Index];

    for (uint32_t Index = Length; Index < NewLength; Index++) // Add empty to new
        NewIDs[Index] = NoID;

    if (Length != 0) // Replace
        delete[] IDs;

    IDs = NewIDs;
    Length = NewLength;
};

void IDList::Shorten()
{
    uint32_t NewLength = Length;
    while (IDs[NewLength - 1].ID == NoID && NewLength > 0)
        NewLength--;

    if (NewLength == Length)
        return;

    IDClass *NewIDs = new IDClass[NewLength];

    for (uint32_t Index = 0; Index < NewLength; Index++) // Copy existing
        NewIDs[Index] = IDs[Index];

    if (Length != 0) // Replace
        delete[] IDs;

    IDs = NewIDs;
    Length = NewLength;
}

bool IDList::Add(IDClass ID, int32_t Index) // Add an item to list, supports -1, fails if occupied
{
    if (Index == -1) // Add to end
        Index = Length;

    if ((uint32_t)Index >= Length) // Expand if Index is too far
        Expand(Index + 1);
    else if (IsValid(Index)) // Occupied
        return false;

    IDs[Index] = ID; // Write
    return true;
};

bool IDList::Add(BaseClass *AddObject, int32_t Index)
{
    if (Index == -1) // Add to end
        Index = Length;

    if ((uint32_t)Index >= Length) // Expand if Index is too far
        Expand(Index + 1);
    else if (IsValid(Index)) // Occupied
        return false;

    IDs[Index] = AddObject->ID; // Write
    return true;
};

bool IDList::Remove(int32_t Index) // Removes object
{
    if ((uint32_t)Index >= Length || Index < 0)
        return false;

    IDs[Index] = NoID;
    Shorten();
    return true;
};

bool IDList::Remove(BaseClass *RemovedObject) // Removes object, even if it is contained more times
{
    for (uint32_t Index = 0; Index < Length; Index++)
    {
        if (At(Index) == RemovedObject || IDs[Index] == RemovedObject->ID)
            Remove(Index);
    }
    return true;
};

bool IDList::RemoveAll()
{
    if (IDs != nullptr)
        delete[] IDs;
    IDs = nullptr;

    Length = 0;
    return true;
};
/*
String IDList::AsString()
{
    String Text = "";
    for (uint32_t Index = 0; Index < Length; Index++)
    {
        if (IsValid(Index))
            Text += String(Index) + " " + String(IDs[Index].ID) + " ";
    }

    return Text;
};*/

template <>
IDList ByteArray::Get(int32_t Index) const
{
    int32_t Start = GetStart(Index);
    IDList List;
    uint8_t DataLength = (uint8_t)Array[Start + sizeof(Types)];

    List.Length = DataLength / sizeof(IDClass);
    List.IDs = new IDClass[List.Length];
    memcpy((void*)List.IDs, Array + sizeof(uint8_t) * 2, DataLength);
    return List;
};

template <>
void ByteArray::Set<IDList>(IDList Data, int32_t Index)
{
    if (Index == -1)
        Index = GetNumberOfValues();

    int32_t Start = GetStart(Index);
    uint8_t DataLength = Data.Length * sizeof(IDClass);

    Resize(Index, DataLength + sizeof(uint8_t), &Start);
    
    Array[Start] = (char)GetType<IDList>();
    memcpy(Array + Start + sizeof(Types), &Data.Length, sizeof(uint8_t));
    memcpy(Array + Start + sizeof(Types) + sizeof(uint8_t), Data.IDs, Data.Length * sizeof(IDClass));
};

template <>
ByteArray::ByteArray(const IDList &Data)
{
    Types Type = Types::IDList;
    Length = sizeof(Types) + sizeof(uint8_t) + Data.Length * sizeof(IDClass);
    Array = new char[Length];

    memcpy(Array, &Type, sizeof(Types));
    memcpy(Array + sizeof(Types), &Data.Length, sizeof(uint8_t));
    memcpy(Array + sizeof(Types) + sizeof(uint8_t), Data.IDs, Data.Length * sizeof(IDClass));
};

template <class C>
C *IDList::Get(int32_t Index)
{
    if (!IsValid(Index))
        return nullptr;

    return At(Index)->As<C>();
}