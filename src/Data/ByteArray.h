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
    template <class C>
    bool operator==(const C Other) const;
    template <class C>
    bool operator!=(const C Other) const;

    ByteArray operator<<(const ByteArray &Data);

    ByteArray SubArray(int32_t Index, int32_t NewLength = -1) const;
    ByteArray AddLength() const;
    ByteArray ExtractMessage();
    ByteArray ExtractPart();

    Types Type() const;
    int32_t SizeOfData() const;   // Size of data
    int32_t SizeWithType() const; // Size of type+length+data

    template <class C>
    C As() const;
    template <class C>
    operator C() const;

    void WriteToFile(const String &FileName);
    String ToHex();
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
    Types Type = GetType<C>();
    Length = sizeof(Types) + sizeof(Data);
    Array = new char[Length];

    memcpy(Array, &Type, sizeof(Types));
    memcpy(Array + sizeof(Types), &Data, sizeof(Data));
};

template <>
ByteArray::ByteArray(const String &Data)
{
    Types Type = GetType<String>();
    uint8_t DataLength = Data.length();
    Length = sizeof(Types) + sizeof(DataLength) + DataLength;
    Array = new char[Length];

    memcpy(Array, &Type, sizeof(Types));
    memcpy(Array + sizeof(Types), &DataLength, sizeof(DataLength));
    memcpy(Array + sizeof(Types) + sizeof(DataLength), Data.c_str(), DataLength);
};

ByteArray::ByteArray(const char *Data, int32_t DataLength)
{
    if (Data == nullptr)
        return;

    Array = new char[DataLength];
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

template <class C>
bool ByteArray::operator==(const C Other) const
{
    if (Type() != GetType<C>())
        return false;
    return As<C>() == Other;
}

template <class C>
bool ByteArray::operator!=(const C Other) const
{
    return !(*this == Other);
}

ByteArray ByteArray::operator<<(const ByteArray &Data)
{
    ByteArray NewArray;
    NewArray.Length = Length + Data.Length;
    NewArray.Array = new char[NewArray.Length];

    memcpy(NewArray.Array, Array, Length);
    memcpy(NewArray.Array + Length, Data.Array, Data.Length);
    return NewArray;
};

template <class C>
C ByteArray::As() const
{
    // IGNORED DUE TO PREVIOUS COMPATIBILITY
    /*
    if (Type() != GetType<C>())
    {
        ReportError(Status::InvalidType, "ByteArray - invalid type conversion");
        return C();
    }
    */
    /*
    if (Length < SizeWithType() || SizeWithType() < 0) // Check if array is long enough
    {
        ReportError(Status::InvalidValue, "ByteArray - value incomplete");
        return C();
    }
    */
    return *(C *)(Array + sizeof(Types));
};

template <>
String ByteArray::As() const
{
    if (Type() != GetType<String>())
    {
        ReportError(Status::InvalidType, "ByteArray - invalid type conversion");
        return "";
    }
    if (Length < SizeWithType() || SizeWithType() < 0) // Check if array is long enough
    {
        ReportError(Status::InvalidValue, "ByteArray - value incomplete");
        return "";
    }

    return String(Array + sizeof(Types) + sizeof(uint8_t), SizeOfData());
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
    return ByteArray(Array + Index, NewLength);
};

ByteArray ByteArray::AddLength() const
{
    return ByteArray((char *)&Length, sizeof(Length)) << *this;
}

ByteArray ByteArray::ExtractMessage()
{
    if (Length < sizeof(uint32_t) || Length < sizeof(uint32_t) + *(uint32_t *)Array)
        return ByteArray();

    ByteArray Message = SubArray(sizeof(uint32_t), *(uint32_t *)Array);
    *this = SubArray(sizeof(uint32_t) + *(uint32_t *)Array);
    return Message;
}

ByteArray ByteArray::ExtractPart()
{
    uint32_t PartLength = SizeWithType();
    if (PartLength < 0)
        return ByteArray();
    ByteArray Part = SubArray(0, PartLength);
    *this = SubArray(PartLength);
    return Part;
}

Types ByteArray::Type() const
{
    if (Length >= sizeof(Types))
        return *(Types *)Array;

    ReportError(Status::InvalidValue, "ByteArray - too short, no type");
    return Types::Undefined;
};

int32_t ByteArray::SizeOfData() const
{
    if (Length < sizeof(Types) + sizeof(uint8_t))
        return -1;
    else if (Type() == Types::Text)
        return *(uint8_t *)(Array + sizeof(Types));
    else if (Type() == Types::IDList)
        return (*(uint8_t *)(Array + sizeof(Types))) * sizeof(IDClass);
    else
        return GetDataSize(Type());
};

int32_t ByteArray::SizeWithType() const
{
    if (SizeOfData() < 0)
        return -1;
    else if (Type() == Types::Text || Type() == Types::IDList)
        return SizeOfData() + sizeof(Types) + sizeof(uint8_t);
    else
        return SizeOfData() + sizeof(Types);
};

void ByteArray::WriteToFile(const String &FileName)
{
    ByteArray Data = AddLength();
    File File = SPIFFS.open("/" + FileName, "w");
    File.print(String(Data.Array, Data.Length));
    File.close();
};

ByteArray ReadFromFile(const String &FileName)
{
    File File = SPIFFS.open("/" + FileName, "r");

    if (!File)
        return ByteArray();

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

String ByteArray::ToHex()
{
    String Text = "";
    for (int32_t Index = 0; Index < Length; Index++)
        Text += String(Array[Index], HEX) + " ";
    return Text;
};