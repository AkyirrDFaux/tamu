class ByteArray
{
private:
    __attribute__((noinline)) void *Get(int32_t Index, size_t Size) const;
    __attribute__((noinline)) void Set(const void *Data, size_t Size, Types Type, int32_t Index);

public:
    char *Array = nullptr;
    uint32_t Length = 0;

    ByteArray() {};
    ByteArray(const ByteArray &Copied);
    ~ByteArray();
    template <class C>
    ByteArray(const C &Data);
    ByteArray(const char *Data, uint32_t DataLength); // Raw data input
    void operator=(const ByteArray &Other);           // Copy

    ByteArray operator<<(const ByteArray &Data) const; // Concatenation, creates new

    int32_t GetStart(int32_t Index) const; // Get byte-index of index-th type
    int32_t GetNumberOfValues() const;     // Length in values/types
    int32_t DataSize(int32_t Pointer) const;
    void Resize(int32_t Index, int32_t NewDataSize, int32_t *Start); // Prepare index-th entry for data of specified size
    Types Type(int32_t Index) const;
    template <class C>
    C Get(int32_t Index) const; // Assumes it's valid, and type is checked
    template <class C>
    void Set(const C &Data, int32_t Index = -1);
    void Copy(ByteArray &Source, int32_t From, int32_t To); // Set, but without specified type

    ByteArray SubArray(int32_t Start, int32_t NewLength = -1) const; // Creates new
    ByteArray CreateMessage() const;                                 // Converts values + adds length
    ByteArray ExtractMessage();                                      // Removes length + converts values

    // void WriteToFile(const String &FileName);
    //Text ToHex();
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
ByteArray::ByteArray(const Text &Data)
{
    Types Type = GetType<Text>();
    Length = sizeof(Types) + sizeof(Data.Length) + Data.Length;
    Array = new char[Length];

    memcpy(Array, &Type, sizeof(Types));
    memcpy(Array + sizeof(Types), &Data.Length, sizeof(Data.Length));
    memcpy(Array + sizeof(Types) + sizeof(Data.Length), Data.Data, Data.Length);
};

ByteArray::ByteArray(const char *Data, uint32_t DataLength)
{
    if (Data == nullptr)
        return;

    Array = new char[DataLength];
    memcpy(Array, Data, DataLength);
    Length = DataLength;
};

void ByteArray::operator=(const ByteArray &Other)
{
    if (this == &Other)
        return;

    if (Length != Other.Length)
    {
        if (Array != nullptr)
            delete[] Array;
        Array = new char[Other.Length];
    }
    if (Other.Length > 0)
        memcpy(Array, Other.Array, Other.Length);
    Length = Other.Length;
};

ByteArray ByteArray::operator<<(const ByteArray &Data) const
{
    ByteArray NewArray;
    NewArray.Length = Length + Data.Length;
    NewArray.Array = new char[NewArray.Length];

    memcpy(NewArray.Array, Array, Length);
    memcpy(NewArray.Array + Length, Data.Array, Data.Length);
    return NewArray;
};

void ByteArray::Resize(int32_t Index, int32_t NewDataSize, int32_t *Start)
{
    if (Array == nullptr) // No data yet
    {
        Length = Index + (sizeof(Types) + NewDataSize); // Padding (n = Index) + Type + Data
        Array = new char[Length];
        for (int32_t Pad = 0; Pad <= Index; Pad++) // Reset values (including new one)
            Array[Pad] = (char)Types::Undefined;
        *Start = Index;
        return;
    }

    int32_t NewSize;
    char *NewArray;

    if (*Start < 0) //  Below length: 1) Create Array 2) Copy 3) Pad 4) Delete old
    {
        int32_t NumOfValues = GetNumberOfValues();
        NewSize = Length + (Index - NumOfValues) + (sizeof(Types) + NewDataSize); // Existing + Padding + New
        NewArray = new char[NewSize];
        memcpy(NewArray, Array, Length);
        for (uint32_t Pad = Length; Pad <= Length + (Index - NumOfValues); Pad++) // Reset values (including new one)
            NewArray[Pad] = (char)Types::Undefined;
        *Start = Length + (Index - NumOfValues);
    }
    else //  Wrong size (Start => 0): 1) Create array 2) Copy before 3) Copy after 4) Delete old
    {
        int32_t PreviousDataSize = DataSize(*Start);

        NewSize = Length - PreviousDataSize + NewDataSize; // Existing + Difference
        NewArray = new char[NewSize];
        memcpy(NewArray, Array, *Start); // Copy previous
        NewArray[*Start] = (char)Types::Undefined;
        int32_t Offset = *Start + sizeof(Types) + PreviousDataSize;
        memcpy(NewArray + *Start + sizeof(Types) + NewDataSize, Array + Offset, Length - Offset); // Copy after
    }

    Length = NewSize;
    delete[] Array;
    Array = NewArray;
}

int32_t ByteArray::GetStart(int32_t Index) const
{
    // Current index
    if (Array == nullptr)
        return -1;

    uint32_t Pointer = 0; // Byte-index of search

    for (int32_t Search = 0; Search <= Index; Search++)
    {
        if (Pointer >= Length) // Check if type is there, fail if overrun
            return -1;

        if (Search == Index) // Search sucessful, end
            return Pointer;

        Pointer += sizeof(Types) + DataSize(Pointer);
    }
    return -1;
};

int32_t ByteArray::GetNumberOfValues() const
{
    if (Array == nullptr)
        return 0;
    // Current index
    uint32_t Pointer = 0; // Byte-index of search
    int32_t Search = 0;
    while (Pointer < Length)
    {
        Pointer += sizeof(Types) + DataSize(Pointer);
        Search++;
    }
    return Search;
};

int32_t ByteArray::DataSize(int32_t Pointer) const
{
    Types Type = (Types)Array[Pointer]; // Get type of block
    switch (Type)                       // Increment by length of block
    {
    case Types::Text:
        return sizeof(uint8_t) + (uint8_t)Array[Pointer + 1];
    case Types::IDList:
        return sizeof(uint8_t) + sizeof(IDClass) * (uint8_t)Array[Pointer + 1];
    default:
        return GetDataSize(Type);
    }
}

Types ByteArray::Type(int32_t Index) const
{
    int32_t Start = GetStart(Index);

    if (Start >= 0)
        return (Types)Array[Start];

    return Types::Undefined; // Invalid
};

void *ByteArray::Get(int32_t Index, size_t Size) const
{
    int32_t Start = GetStart(Index);
    if (Start < 0)
        return nullptr;
    return Array + Start + sizeof(Types);
}

template <class C>
C ByteArray::Get(int32_t Index) const
{
    C Value;
    void *Pointer = Get(Index, sizeof(C));
    if (Pointer)
        memcpy((void *)&Value, Pointer, sizeof(C));
    else
        Value = C();
    return Value;
}

template <>
Text ByteArray::Get(int32_t Index) const
{
    int32_t Start = GetStart(Index);
    if (Start < 0) // Invalid data
        return Text();
    Text Text;
    Text.Length = (uint8_t)Array[Start + sizeof(Types)];
    Text.Data = new char[Text.Length];
    memcpy(Text.Data, Array + Start + sizeof(Types) + sizeof(uint8_t), Text.Length);
    return Text;
};

void ByteArray::Set(const void *Data, size_t Size, Types Type, int32_t Index)
{
    if (Index == -1)
        Index = GetNumberOfValues();

    int32_t Start = GetStart(Index);
    Types CurrentType = (Start >= 0) ? (Types)Array[Start] : Types::Undefined;

    // Check if we can reuse existing space
    if (CurrentType == Types::Undefined || GetDataSize(CurrentType) != GetDataSize(Type))
    {
        Resize(Index, Size, &Start);
    }

    Array[Start] = (char)Type;
    memcpy(Array + Start + sizeof(Types), Data, Size);
}

template <class C>
void ByteArray::Set(const C &Data, int32_t Index)
{
    Set(&Data, sizeof(C), GetType<C>(), Index);
};

template <>
void ByteArray::Set(const Text &Data, int32_t Index)
{
    if (Index == -1)
        Index = GetNumberOfValues();

    int32_t Start = GetStart(Index);

    Resize(Index, Data.Length + sizeof(uint8_t), &Start);
    Array[Start] = (char)GetType<Text>();

    memcpy(Array + Start + sizeof(Types), &Data.Length, sizeof(Data.Length));
    memcpy(Array + Start + sizeof(Types) + sizeof(uint8_t), Data.Data, Data.Length);
};

void ByteArray::Copy(ByteArray &Source, int32_t From, int32_t To)
{
    if (To == -1)
        To = GetNumberOfValues();

    int32_t FromStart = Source.GetStart(From);
    int32_t ToStart = GetStart(To);
    Types TargetType = Types::Undefined;
    Types CurrentType = Types::Undefined;

    if (FromStart >= 0) // Get Type (faster)
        TargetType = (Types)Source.Array[FromStart];
    if (TargetType == Types::Undefined)
        return;
    if (ToStart >= 0)
        CurrentType = (Types)Array[ToStart];

    if (CurrentType == TargetType) // Can simply copy, same type
    {
        memcpy(Array + ToStart + sizeof(Types), Source.Array + FromStart + sizeof(Types), Source.DataSize(FromStart));
        return;
    }
    else if (CurrentType == Types::Undefined || GetDataSize(CurrentType) != GetDataSize(TargetType)) // Wrong length
    {
        Resize(To, Source.DataSize(FromStart), &ToStart);
    }

    Array[ToStart] = (char)TargetType;
    memcpy(Array + ToStart + sizeof(Types), Source.Array + FromStart + sizeof(Types), Source.DataSize(FromStart));
}

ByteArray ByteArray::SubArray(int32_t Start, int32_t NewLength) const
{
    if (NewLength == -1)
        NewLength = Length - Start;
    return ByteArray(Array + Start, NewLength);
};

ByteArray ByteArray::CreateMessage() const
{
    // TODO convert fixed types
    ByteArray Buffer = *this;
#if USE_FIXED_POINT == 1
    uint32_t Pointer = 0;
    while (Pointer < Buffer.Length)
    {
        float TempFloat; //%4 byte alignment safe
        Number TempNumber;

        switch ((Types)Buffer.Array[Pointer])
        {
        case Types::Coord2D:
            memcpy(&TempNumber, Buffer.Array + Pointer + 13, sizeof(Number));
            TempFloat = (float)TempNumber;
            memcpy(Buffer.Array + Pointer + 13, &TempFloat, sizeof(float));
        case Types::Vector3D:
            memcpy(&TempNumber, Buffer.Array + Pointer + 9, sizeof(Number));
            TempFloat = (float)TempNumber;
            memcpy(Buffer.Array + Pointer + 9, &TempFloat, sizeof(float));
        case Types::Vector2D:
            memcpy(&TempNumber, Buffer.Array + Pointer + 5, sizeof(Number));
            TempFloat = (float)TempNumber;
            memcpy(Buffer.Array + Pointer + 5, &TempFloat, sizeof(float));
        case Types::Number:
            memcpy(&TempNumber, Buffer.Array + Pointer + 1, sizeof(Number));
            TempFloat = (float)TempNumber;
            memcpy(Buffer.Array + Pointer + 1, &TempFloat, sizeof(float));
            break;
        default:
            break;
        }

        Pointer += sizeof(Types) + Buffer.DataSize(Pointer);
    }
#endif
    return ByteArray((char *)&Length, sizeof(Length)) << Buffer;
}

ByteArray ByteArray::ExtractMessage()
{
    if (Length < sizeof(uint32_t))
        return ByteArray();

    uint32_t MessageLength;
    memcpy(&MessageLength, Array, sizeof(uint32_t));

    if (Length < sizeof(uint32_t) + MessageLength)
        return ByteArray();

    ByteArray Message = SubArray(sizeof(uint32_t), MessageLength);

    int32_t NewLength = Length - MessageLength - sizeof(uint32_t);
    char *NewArray = nullptr;
    if (NewLength > 0)
    {
        NewArray = new char[NewLength];
        memcpy(NewArray, Array + sizeof(uint32_t) + MessageLength, NewLength);
    }
    delete[] Array;
    Array = NewArray;
    Length = NewLength;

#if USE_FIXED_POINT == 1
    uint32_t Pointer = 0;
    while (Pointer < Message.Length)
    {
        float TempFloat; //%4 byte alignment safe
        Number TempNumber;

        switch ((Types)Message.Array[Pointer])
        {
        case Types::Coord2D:
            memcpy(&TempFloat, Message.Array + Pointer + 13, sizeof(float));
            TempNumber = (Number)TempFloat;
            memcpy(Message.Array + Pointer + 13, &TempNumber, sizeof(Number));
        case Types::Vector3D:
            memcpy(&TempFloat, Message.Array + Pointer + 9, sizeof(float));
            TempNumber = (Number)TempFloat;
            memcpy(Message.Array + Pointer + 9, &TempNumber, sizeof(Number));
        case Types::Vector2D:
            memcpy(&TempFloat, Message.Array + Pointer + 5, sizeof(float));
            TempNumber = (Number)TempFloat;
            memcpy(Message.Array + Pointer + 5, &TempNumber, sizeof(Number));
        case Types::Number:
            memcpy(&TempFloat, Message.Array + Pointer + 1, sizeof(float));
            TempNumber = (Number)TempFloat;
            memcpy(Message.Array + Pointer + 1, &TempNumber, sizeof(Number));
            break;
        default:
            break;
        }

        Pointer += sizeof(Types) + Message.DataSize(Pointer);
    }
#endif

    return Message;
}

/*
void ByteArray::WriteToFile(const String &FileName)
{
    ByteArray Data = AddLength();
    File File = LittleFS.open("/" + FileName, "w");
    File.print(String(Data.Array, Data.Length));
    File.close();
};

ByteArray ReadFromFile(const String &FileName)
{
    File File = LittleFS.open("/" + FileName, "r");

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
*/

/*
Text ByteArray::ToHex()
{
    // 2 chars per byte + 1 space + null terminator
    char* hexBuffer = (char*)malloc(Length * 3 + 1);
    if (!hexBuffer) return Text("");

    for (uint32_t i = 0; i < Length; i++) {
        sprintf(hexBuffer + (i * 3), "%02X ", Array[i]);
    }
    
    // Convert to your 'Text' type (assuming it handles the copy/pointer)
    Text result = Text(hexBuffer); 
    free(hexBuffer);
    return result;
};*/
