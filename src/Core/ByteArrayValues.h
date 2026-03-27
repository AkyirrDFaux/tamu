template <class C>
Getter<C> ByteArray::Get(const Reference &Location) const
{
    const void *ptr = Get(Location, GetType<C>());
    if (ptr == nullptr)
        return {false, C()}; 

    return {true, *(C *)ptr};
}

template <>
Getter<Text> ByteArray::Get(const Reference &Location) const
{
    FindResult Result = Find(Location);
    if (Result.Type != Types::Text || Result.Header == -1)
    {
        return {false, Text(nullptr, 0)};
    }

    const Header &Head = HeaderArray[Result.Header];
    // Return pointer to internal buffer + length from metadata
    return {true, Text((const char*)Result.Value, Head.Length)};
}

template <>
Getter<Reference> ByteArray::Get(const Reference &Location) const
{
    FindResult Result = Find(Location);

    if (Result.Header == -1 || Result.Value == nullptr)
        return {false, Reference()};

    const Header &Head = HeaderArray[Result.Header];
    Reference ref;

    if (Result.Type == Types::Reference)
    {
        // 1. Global Layout: [Net][Group][Device][...Path...]
        // Stored length in Header is (3 + PathLen)
        uint8_t pathLen = (Head.Length >= 3) ? (uint8_t)(Head.Length - 3) : 0;
        
        // Directly set Metadata: Global Bit (0x80) | Path Length
        ref.Metadata = 0x80 | (pathLen & 0x7F);

        // Copy IDs and Path into the Data union
        uint16_t copySize = (Head.Length > sizeof(ref.Data)) ? sizeof(ref.Data) : Head.Length;
        memcpy(ref.Data, Result.Value, copySize);
        
        return {true, ref};
    }
    else if (Result.Type == Types::Path)
    {
        // 2. Local Layout: [...Path...]
        // Stored length in Header is exactly PathLen
        uint8_t pathLen = (uint8_t)Head.Length;

        // Directly set Metadata: Global Bit is 0 | Path Length
        ref.Metadata = (pathLen & 0x7F);

        // Copy raw path bytes into the Path segment of the union
        uint16_t copySize = (Head.Length > MAX_REF_LENGTH) ? MAX_REF_LENGTH : Head.Length;
        memcpy(ref.Path, Result.Value, copySize);
        
        return {true, ref};
    }

    return {false, Reference()};
}

void ByteArray::Set(const void *Data, size_t Size, Types Type, const Reference &Location)
{
    FindResult Found = Find(Location);

    // Handle packed sub-access (e.g. accessing a byte inside a Vec3)
    if (Found.Header == -2)
    { 
        if (Found.Value != nullptr)
            memcpy(Found.Value, Data, Size);
        return;
    }

    uint16_t HIdx;
    if (Found.Header == -1)
    {
        HIdx = Insert(Location, (uint16_t)Size);
    }
    else
    {
        HIdx = (uint16_t)Found.Header;
        if (HeaderArray[HIdx].Length != Size)
        {
            ResizeData(HIdx, (uint16_t)Size);
        }
    }

    // Update metadata and copy data
    HeaderArray[HIdx].Type = Type;
    // ResizeData already updated HeaderArray[HIdx].Length
    
    if (Size > 0 && Data != nullptr)
    {
        memcpy(Array + HeaderArray[HIdx].Pointer, Data, Size);
    }
}

template <class C>
void ByteArray::Set(const C &Data, const Reference &Location)
{
    Set(&Data, sizeof(C), GetType<C>(), Location);
}

template <>
void ByteArray::Set(const Text &Data, const Reference &Location)
{
    if (Data.Data == nullptr) return;
    Set(Data.Data, Data.Length, Types::Text, Location);
}

template <>
void ByteArray::Set(const Reference &Data, const Reference &Location)
{
    if(Data.IsGlobal())
        Set(&Data.Data, Data.PathLen() + 3, Types::Reference, Location);
    else
        Set(&Data.Path, Data.PathLen(), Types::Path, Location);  
}