bool IDList::IsValid(int32_t Index, ObjectTypes Filter) // Returns if object at index is valid
{
    return (At(Index) != nullptr && (Filter == ObjectTypes::Undefined || At(Index)->Type == Filter));
};

int32_t IDList::FirstValid(ObjectTypes Filter, int32_t Start)
{
    int32_t Index = Start;
    while (Index <= Length && (!IsValid(Index) || (Filter != ObjectTypes::Undefined && At(Index)->Type != Filter)))
        Index++;
    return Index;
};

void IDList::Iterate(int32_t *Index, ObjectTypes Filter)
{
    (*Index)++;
    // Skip to next if: A)Is invalid; B)Filter is not undefined and Type of object isn't one filtered
    while (*Index <= Length && (!IsValid(*Index) || (Filter != ObjectTypes::Undefined && At(*Index)->Type != Filter)))
        (*Index)++;
};

bool IDList::Remove(BaseClass *RemovedObject) // Removes object, even if it is contained more times
{
    for (int32_t Index = 0; Index < Length; Index++)
    {
        if (At(Index) == RemovedObject || IDs[Index] == RemovedObject->ID)
            Remove(Index);
    }
    return true;
};

bool IDList::Update(int32_t Index)
{
    if (IsValidID(Index) == false)
        return false;

    Object[Index] = Objects.Find(IDs[Index]);

    return Object[Index] != nullptr;
};

bool IDList::Add(BaseClass *AddObject, int32_t Index)
{
    if (Index == -1) // Add to end
        Index = Length;

    if (Index >= Length) // Expand if Index is too far
        Expand(Index + 1);
    else if (IsValidID(Index)) // Occupied
        return false;

    IDs[Index] = AddObject->ID; // Write
    Object[Index] = AddObject;
    return true;
};

template <class C>
C *IDList::Get(int32_t Index)
{
    if (!IsValid(Index))
        return nullptr;

    return At(Index)->As<C>();
}

Types IDList:: TypeAt(int32_t Index)
{
    if (At(Index) == nullptr || (IsValidID(Index) && IDs->Sub() == 0)) //Invalid or reference to main obj
        return Types::Undefined;

    return At(Index)->Values.TypeAt(IDs[Index].ValueIndex());
};
void *IDList::ValueAt(int32_t Index) {
    if (At(Index) == nullptr || (IsValidID(Index) && IDs->Sub() == 0)) //Invalid or reference to main obj
        return nullptr;
    
     return At(Index)->Values[IDs[Index].ValueIndex()];
};

// DATALIST

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

        return Objects.Find(*At<IDClass>(Index))->Values[At<IDClass>(Index)->ValueIndex()];
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

        return Objects.Find(*At<IDClass>(Index))->Values.TypeAt(At<IDClass>(Index)->ValueIndex());
    }
    else
        return Type[Index];
};