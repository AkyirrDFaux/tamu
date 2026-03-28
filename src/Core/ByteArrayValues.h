const void *ByteArray::Get(const Reference &Location, Types Type) const
{
    FindResult Result = Find(Location, false);
    if (Result.Header == -1 || Result.Type != Type)
        return nullptr;
    return Result.Value;
}

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
    FindResult Result = Find(Location, false);
    if (Result.Header == -1 || Result.Type != Types::Text)
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

    // 1. Basic validation
    if (Result.Header == -1 || Result.Type != Types::Reference)
        return {false, Reference()};

    // 2. Initialize a clean Reference (zeros out the union)
    Reference ref;

    // 3. Determine how much to copy
    // We only copy what is available in the ByteArray, 
    // but never more than the size of our struct (16 bytes).
    uint16_t storedSize = HeaderArray[Result.Header].Length;
    uint16_t copySize = (storedSize > sizeof(Reference)) ? sizeof(Reference) : storedSize;

    // 4. Copy the raw bytes directly into the struct
    // This works because the struct is packed and starts with Metadata
    memcpy(&ref, Result.Value, copySize);

    return {true, ref};
}

void ByteArray::Set(const void *Data, size_t Size, Types Type, const Reference &Location)
{
    FindResult Found = Find(Location);

    // Handle packed sub-access (e.g. accessing a byte inside a Vec3)
    if (Found.Header == -2)
    { 
        if (Found.Value != nullptr && Found.Type == Type)
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
    Set(&Data, Data.PathLen() + 4, Types::Reference, Location);
}