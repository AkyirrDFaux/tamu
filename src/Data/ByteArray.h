class ByteArray
{
public:
    char *Array = nullptr;
    int32_t Length = 0;

    ByteArray() {};
    ByteArray(const ByteArray &Copied);
    ~ByteArray();
    template <class C>
    ByteArray(const C &Data);
    ByteArray(const char *Data, int32_t DataLength);
    void operator=(const ByteArray &Other);

    ByteArray operator<<(const ByteArray &Data);
    ByteArray operator>>(const ByteArray &Data);

    ByteArray SubArray(int32_t Index, int32_t NewLength = -1) const;

    template <class C>
    C As() const;

    template <class C>
    operator C() const;

    template <class C>
    C Data() const;

    Types Type() const;
    size_t SizeOfData() const;

    void WriteToFile(const String &FileName);
};

ByteArray::ByteArray(const ByteArray &Copied)
{
    Length = Copied.Length;
    Array = new char[Length];
    memcpy(Array, Copied.Array, Length);
};

ByteArray::~ByteArray()
{
    if (Array != nullptr)
        delete[] Array;
};

template <class C>
ByteArray::ByteArray(const C &Data)
{
    Array = new char[sizeof(Data)];
    memcpy(Array, &Data, sizeof(Data));
    Length = sizeof(Data);
};

template <>
ByteArray::ByteArray(const String &Data)
{
    u_int8_t DataLength = Data.length();
    Array = new char[sizeof(DataLength) + DataLength];

    memcpy(Array, &DataLength, sizeof(DataLength));
    memcpy(Array + sizeof(DataLength), Data.c_str(), DataLength);

    Length = DataLength + sizeof(DataLength);
};

ByteArray::ByteArray(const char *Data, int32_t DataLength)
{
    Array = new char[DataLength];
    if (Data != nullptr)
        memcpy(Array, Data, DataLength);
    Length = DataLength;
};

void ByteArray::operator=(const ByteArray &Other)
{
    if (Length != Other.Length)
    {
        if (Array != nullptr)
            delete[] Array;
        Array = new char[Other.Length];
    }
    memcpy(Array, Other.Array, Other.Length);
    Length = Other.Length;
};

ByteArray ByteArray::operator<<(const ByteArray &Data)
{
    ByteArray NewArray;
    NewArray.Length = Length + Data.Length;
    NewArray.Array = new char[NewArray.Length];

    memcpy(NewArray.Array, Array, Length);
    memcpy(NewArray.Array + Length, Data.Array, Data.Length);
    return NewArray;
};

ByteArray ByteArray::operator>>(const ByteArray &Data)
{
    ByteArray NewArray;
    NewArray.Length = Length + Data.Length;
    NewArray.Array = new char[NewArray.Length];

    memcpy(NewArray.Array, Data.Array, Data.Length);
    memcpy(NewArray.Array + Data.Length, Array, Length);
    return NewArray;
};

template <class C>
C ByteArray::As() const
{
    if (Length < sizeof(C)) // Check if array is long enough
    {
        ReportError(Status::InvalidValue, "ByteArray conversion failed");
        return C();
    }

    C Part;
    memcpy(&Part, Array, sizeof(C));
    return Part;
};

template <>
String ByteArray::As() const
{
    u_int8_t TextLength = *this;
    if (Length - sizeof(u_int8_t) < TextLength)
        return "";

    String Part = String(Array + sizeof(u_int8_t), TextLength);
    return Part;
};

template <class C>
ByteArray::operator C() const
{
    return As<C>();
};

ByteArray ByteArray::SubArray(int32_t Index, int32_t NewLength) const
{
    if (NewLength == -1)
        NewLength = Length - Index;
    ByteArray NewArray(Array + Index, NewLength);
    return NewArray;
};

Types ByteArray::Type() const
{
    Types Type = Types::Undefined;
    memcpy(&Type, Array, sizeof(Types));
    return Type;
};

size_t ByteArray::SizeOfData() const
{
    int8_t Size = GetValueSize(Type());
    if (Size >= 0)
        return Size;

    switch (Type())
    {
    case Types::Text:
        return SubArray(sizeof(Types)).As<uint8_t>() + sizeof(uint8_t);
    case Types::IDList:
        return SubArray(sizeof(Types)).As<int8_t>() * (sizeof(uint32_t) + sizeof(uint8_t)) + sizeof(uint8_t);
    default:
        ReportError(Status::InvalidType, "Unknown length data, Type:" + String((uint8_t)Type()));
        return Length - sizeof(Types);
    }
};

template <class C>
C ByteArray::Data() const
{
    return SubArray(sizeof(Types)).As<C>();
};

void ByteArray::WriteToFile(const String &FileName)
{
    ByteArray Data = ByteArray(Length) << *this;
    File File = SPIFFS.open("/" + FileName, "w");
    File.print(String(Data.Array, Data.Length));
    File.close();
};

ByteArray ReadFromFile(const String &FileName)
{
    File File = SPIFFS.open("/" + FileName, "r");

    // Forgot to check if it opened/exists

    int32_t ReadLength = 0;

    ByteArray Length = ByteArray((int32_t)0);
    ReadLength = File.readBytes(Length.Array, sizeof(int32_t));

    if (ReadLength < sizeof(int32_t))
    {
        ReportError(Status::FileError, "Corrupted file");
        return ByteArray();
    }

    ByteArray Data = ByteArray(nullptr, Length.As<int32_t>());

    ReadLength = File.readBytes(Data.Array, Data.Length);
    
    if (ReadLength < Data.Length)
    {
        ReportError(Status::FileError, "Corrupted file");
        return ByteArray();
    }

    File.close();
    return Data;
};
