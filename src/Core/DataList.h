//STRINGS & VECTOR3 ARE BROKEN

class DataList
{
public:
    Types *Type = nullptr;
    void **Data = nullptr;
    int32_t Length = 0;

    ~DataList() { DeleteAll(); };

    void *operator[](int32_t Index) const;
    template <class C>
    C *At(int32_t Index) const;
    bool IsValid(int32_t Index, Types TypeCheck = Types::Undefined) const;

    void Expand(int32_t NewLength);
    void Shorten();

    template <class C>
    bool Add(C AddObject, int32_t Index = -1);
    bool Delete(int32_t Index);
    void DeleteAll();
};

void *DataList::operator[](int32_t Index) const
{
    if (IsValid(Index))
        return nullptr;

    if (Index < 0) // From end
        Index = Length + Index;

    return Data[Index];
};

template <class C>
C *DataList::At(int32_t Index) const // Returns address or nullptr if invalid
{
    if (IsValid(Index,GetType<C>()) == false)
        return nullptr;

    if (Index < 0) // From end
        Index = Length + Index;

    return (C*)Data[Index];
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
        Data[Index] = nullptr;
        //Incomplete deletion
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

    //What if already nullptr?
    delete[] Data;
    delete[] Type;

    Data = nullptr;
    Type = nullptr;
    Length = 0;
};