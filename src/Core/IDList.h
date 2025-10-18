class IDList
{
public:
    BaseClass **Object = nullptr;
    IDClass *IDs = nullptr;
    int8_t Length = 0;

    ~IDList() { RemoveAll(); };

    IDList() {};
    IDList(const IDList &Copied);
    void operator=(const IDList &Copied);

    BaseClass *operator[](int32_t Index);
    BaseClass *At(int32_t Index);

    bool IsValid(int32_t Index, Types Filter = Types::Undefined);
    bool IsValidID(int32_t Index) const;

    int32_t Find(BaseClass *TestObject);
    int32_t FirstValid(Types Filter = Types::Undefined, int32_t Start = 0);
    void Iterate(int32_t *Index, Types Filter = Types::Undefined);

    void Expand(int8_t NewLength);
    void Shorten();

    bool Add(IDClass ID, int32_t Index = -1);
    bool Add(BaseClass *AddObject, int32_t Index);
    bool Remove(int32_t Index);
    bool Remove(BaseClass *RemovedObject);
    bool RemoveAll();

    bool Update(int32_t Index);

    template <class C>
    C *Get(int32_t Index);

    String AsString();
};

IDList::IDList(const IDList &Copied)
{
    Length = Copied.Length;
    IDs = new IDClass[Length];
    Object = new BaseClass *[Length];
    memcpy(IDs, Copied.IDs, Length * sizeof(IDClass));
    memcpy(Object, Copied.Object, Length * sizeof(BaseClass *));
};

void IDList::operator=(const IDList &Copied)
{
    Length = Copied.Length;
    IDs = new IDClass[Length];
    Object = new BaseClass *[Length];
    memcpy(IDs, Copied.IDs, Length * sizeof(IDClass));
    memcpy(Object, Copied.Object, Length * sizeof(BaseClass *));
};

BaseClass *IDList::operator[](int32_t Index)
{
    return At(Index);
};

BaseClass *IDList::At(int32_t Index) // Returns address or nullptr if invalid
{
    if (Index >= Length || Index < 0)
        return nullptr;

    if (Object[Index] == nullptr)
        Update(Index);

    return Object[Index];
};

bool IDList::IsValidID(int32_t Index) const
{
    if (Index >= Length || Index < 0)
        return false;

    return IDs[Index].Main() != NoID;
};

int32_t IDList::Find(BaseClass *TestObject) // Returns index of object address, -1 if fails
{
    for (int32_t Index = 0; Index < Length; Index++)
    {
        if (At(Index) == TestObject)
            return Index;
    }
    return -1;
};

void IDList::Expand(int8_t NewLength) // Expands list to new length
{
    if (NewLength <= Length)
        return;

    BaseClass **NewObject = new BaseClass *[NewLength]; // Make larger array
    IDClass *NewIDs = new IDClass[NewLength];

    for (int32_t Index = 0; Index < Length; Index++) // Copy existing
    {
        NewObject[Index] = Object[Index];
        NewIDs[Index] = IDs[Index];
    }

    for (int32_t Index = Length; Index < NewLength; Index++) // Add empty to new
    {
        NewObject[Index] = nullptr;
        NewIDs[Index] = NoID;
    }

    if (Length != 0) // Replace
    {
        delete[] Object;
        delete[] IDs;
    }

    Object = NewObject;
    IDs = NewIDs;
    Length = NewLength;
};

void IDList::Shorten()
{
    int32_t NewLength = Length;
    while (IDs[NewLength - 1].ID == NoID && NewLength > 0)
        NewLength--;

    if (NewLength == Length)
        return;

    BaseClass **NewObject = new BaseClass *[NewLength]; // New Array
    IDClass *NewIDs = new IDClass[NewLength];

    for (int32_t Index = 0; Index < NewLength; Index++) // Copy existing
    {
        NewObject[Index] = Object[Index];
        NewIDs[Index] = IDs[Index];
    }

    if (Length != 0) // Replace
    {
        delete[] Object;
        delete[] IDs;
    }

    Object = NewObject;
    IDs = NewIDs;
    Length = NewLength;
}

bool IDList::Add(IDClass ID, int32_t Index) // Add an item to list, supports -1, fails if occupied
{
    if (Index == -1) // Add to end
        Index = Length;

    if (Index >= Length) // Expand if Index is too far
        Expand(Index + 1);
    else if (IsValidID(Index)) // Occupied
        return false;

    IDs[Index] = ID; // Write
    Update(Index);
    return true;
};

bool IDList::Remove(int32_t Index) // Removes object
{
    if (Index >= Length || Index < 0)
        return false;

    Object[Index] = nullptr;
    IDs[Index] = NoID;
    Shorten();
    return true;
};

bool IDList::RemoveAll()
{
    if (Object != nullptr)
        delete[] Object;
    Object = nullptr;

    if (IDs != nullptr)
        delete[] IDs;
    IDs = nullptr;

    Length = 0;
    return true;
};

String IDList::AsString()
{
    String Text = "";
    for (int Index = 0; Index < Length; Index++)
    {
        if (IsValidID(Index))
            Text += String(Index) + " " + String(IDs[Index].ID) + " ";
    }

    return Text;
};

template <>
IDList ByteArray::As() const
{
    if (Type() != GetType<IDList>())
    {
        ReportError(Status::InvalidType, "ByteArray - invalid type conversion");
        return IDList();
    }
    if (Length < SizeWithType() || SizeWithType() < 0) // Check if array is long enough
    {
        ReportError(Status::InvalidValue, "ByteArray - value incomplete");
        return IDList();
    }

    IDList List;
    List.Length = SizeOfData()/sizeof(IDClass);
    List.IDs = new IDClass[List.Length];
    memcpy(List.IDs, Array + sizeof(uint8_t) * 2, SizeOfData());
    return List;
};

template <>
ByteArray::ByteArray(const IDList &Data)
{
    Types Type = GetType<IDList>();
    Length = sizeof(Types) + sizeof(uint8_t) + Data.Length * sizeof(IDClass);
    Array = new char[Length];

    memcpy(Array, &Type, sizeof(Types));
    memcpy(Array + sizeof(Types), &Data.Length, sizeof(uint8_t));
    memcpy(Array + sizeof(Types) + sizeof(uint8_t), Data.IDs, Data.Length * sizeof(IDClass));
};
