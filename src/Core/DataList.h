template <class C>
C *DataList::At(int32_t Index) const // Returns address or nullptr if invalid
{
    if (Index < 0) // From end
        Index = Length + Index;

    if (IsValid(Index) == false) // Anything there?
        return nullptr;

    if (IsValid(Index, GetType<C>())) // Local
        return (C *)Data[Index];
    else if (Type[Index] == Types::ID) // Remote
    {
        if (Objects.IsValid(*At<IDClass>(Index)) == false)
            return nullptr;

        return Objects.At(*At<IDClass>(Index))->Values.At<C>(At<IDClass>(Index)->ValueIndex());
    }

    return nullptr;
};

bool DataList::IsValid(int32_t Index, Types TypeCheck) const // Returns if object at index is valid
{
    if (Index >= Length || -Index > Length)
        return false;

    if (Index < 0) // From end
        Index = Length + Index;

    return ((Type[Index] == TypeCheck || TypeCheck == Types::Undefined) && Data[Index] != nullptr);
};

void DataList::Expand(int32_t NewLength) // Expands list to new length
{
    if (NewLength <= Length)
        return;

    void **NewData = new void *[NewLength]; // Make larger array
    Types *NewType = new Types[NewLength];

    for (int32_t Index = 0; Index < Length; Index++) // Copy existing
    {
        NewData[Index] = Data[Index];
        NewType[Index] = Type[Index];
    }

    for (int32_t Index = Length; Index < NewLength; Index++) // Add empty to new
    {
        NewData[Index] = nullptr;
        NewType[Index] = Types::Undefined;
    }

    if (Length != 0) // Replace
    {
        delete[] Data;
        delete[] Type;
    }

    Data = NewData;
    Type = NewType;
    Length = NewLength;
};

void DataList::Shorten()
{
    int32_t NewLength = Length;
    while (IsValid(NewLength - 1) == false && NewLength > 0)
        NewLength--;

    if (NewLength == Length)
        return;

    void **NewData = new void *[NewLength]; // New Array
    Types *NewType = new Types[NewLength];

    for (int32_t Index = 0; Index < Length; Index++) // Copy existing
    {
        NewData[Index] = Data[Index];
        NewType[Index] = Type[Index];
    }

    if (Length != 0) // Replace
    {
        delete[] Data;
        delete[] Type;
    }

    Data = NewData;
    Type = NewType;
    Length = NewLength;
}

template <class C>
bool DataList::Add(C AddData, int32_t Index) // Add an item to list, supports -1, fails if occupied
{
    if (Index == -1) // Add to end
        Index = Length;

    if (Index >= Length) // Expand if Index is too far
        Expand(Index + 1);
    else if (IsValid(Index) == true) // Occupied
        return false;

    Data[Index] = new C;
    Type[Index] = GetType<C>();
    *At<C>(Index) = AddData;

    return true;
};

bool DataList::Delete(int32_t Index) // Removes object
{
    if (IsValid(Index) == true)
    {
        // Deletion
        if (Type[Index] < Types::Integer && Type[Index] >= Types::Byte)
            delete (uint8_t *)Data[Index];
        else if (Type[Index] < Types::Vector2D)
            delete (uint32_t *)Data[Index];
        else if (Type[Index] == Types::Vector2D)
            delete (Vector2D *)Data[Index];
        else if (Type[Index] == Types::Vector3D)
            delete (Vector3D *)Data[Index];
        else if (Type[Index] == Types::Coord2D)
            delete (Coord2D *)Data[Index];
        else if (Type[Index] == Types::Text)
            delete (String *)Data[Index];

        Data[Index] = nullptr;
        Type[Index] = Types::Undefined;
        Shorten();
        return true;
    }
    return false;
};

void DataList::DeleteAll()
{
    for (int32_t Index = 0; Index < Length; Index++)
        Delete(Index);

    if (Data != nullptr)
        delete[] Data;
    if (Type != nullptr)
        delete[] Type;

    Data = nullptr;
    Type = nullptr;
    Length = 0;
};

void *DataList::operator[](int32_t Index) const
{
    if (Index >= Length || -Index > Length)
        return nullptr;

    if (Index < 0) // From end
        Index = Length + Index;

    if (Type[Index] == Types::ID)
    {
        if (Objects.IsValid(*At<IDClass>(Index)) == false)
            return nullptr;

        return Objects.At(*At<IDClass>(Index))->Values[At<IDClass>(Index)->ValueIndex()];
    }
    else
        return Data[Index];
};

Types DataList::TypeAt(int32_t Index) const // Returns address or nullptr if invalid
{
    if (IsValid(Index) == false)
        return Types::Undefined;

    if (Index < 0) // From end
        Index = Length + Index;

    if (Type[Index] == Types::ID)
    {
        if (Objects.IsValid(*At<IDClass>(Index)) == false)
            return Types::Undefined;

        return Objects.At(*At<IDClass>(Index))->Values.TypeAt(At<IDClass>(Index)->ValueIndex());
    }
    else
        return Type[Index];
};

template <class C>
bool DataList::Write(C AddObject, int32_t Index)
{
    if (Index < 0) // From end
        Index = Length + Index;

    if (IsValid(Index, GetType<C>())) // Perfect match
        *At<C>(Index) = AddObject;
    else if (Add(AddObject, Index)) // Nothing there yet
        return true;
    else
    {
        Delete(Index);
        return Add(AddObject, Index);
    }

    return true;
}