const void *ByteArray::Get(const Path &Location, Types Type) const
{
    FindResult Result = Find(Location);
    if (Result.Type != Type)
        return nullptr;
    return Result.Value;
}

// 1. Standard Template: Returns the value inside the Getter struct
template <class C>
Getter<C> ByteArray::Get(const Path &Location) const
{
    // Use the internal check-and-find function
    const void *ptr = Get(Location, GetType<C>());

    if (ptr == nullptr)
        return {false, C()}; // Return failed status and default-constructed value

    // Success: return value by copying from the buffer
    return {true, *(C *)ptr};
}

template <>
Getter<Text> ByteArray::Get(const Path &Location) const
{
    // 1. Find the object in the buffer
    FindResult Result = Find(Location);

    // 2. Validate Type and ensure the header exists
    if (Result.Type != Types::Text || Result.Header == -1)
    {
        return {false, Text(nullptr, 0)};
    }

    // 3. Access the Header to get the true stored length
    const Header &Head = HeaderArray[Result.Header];

    // 4. Return the pointer from the result and the length from the header
    // We do not need to copy the string, just point to its location in the 'Array'
    return {true, Text((const char*)Result.Value, Head.Length)};
}

template <>
Getter<Reference> ByteArray::Get(const Path &Location) const
{
    FindResult Result = Find(Location);

    if (Result.Type != Types::Reference || Result.Header == -1)
        return {false, Reference(0, 0, 0)};

    const Header &Head = HeaderArray[Result.Header];
    if (Head.Length < 3)
        return {false, Reference(0, 0, 0)};

    const uint8_t *data = (const uint8_t *)Result.Value;
    uint8_t net = data[0];
    uint8_t group = data[1];
    uint8_t device = data[2];

    // FIX: Reference Location is a "view" into the existing Array
    // Do NOT memcpy into path.Indexing (which is a const pointer)
    Path path(data + 3, (uint8_t)(Head.Length - 3));

    return {true, Reference(net, group, device, path)};
}

void ByteArray::Set(const void *Data, size_t Size, Types Type, const Path &Location)
{
    FindResult Found = Find(Location);

    if (Found.Header == -2)
    { // Packed sub-access
        if (Found.Value != nullptr)
            memcpy(Found.Value, Data, Size);
        return;
    }

    if (Found.Header == -1)
    {
        Found.Header = Insert(Location, Size);
    }
    else if (HeaderArray[Found.Header].Length != Size)
    {
        ResizeData(Found.Header, Size);
    }

    // Update metadata and copy data
    HeaderArray[Found.Header].Type = Type;
    HeaderArray[Found.Header].Length = Size;
    if (Size > 0 && Data != nullptr)
    {
        memcpy(Array + HeaderArray[Found.Header].Pointer, Data, Size);
    }
}

template <class C>
void ByteArray::Set(const C &Data, const Path &Location)
{
    Set(&Data, sizeof(C), GetType<C>(), Location);
};

template <>
void ByteArray::Set(const Text &Data, const Path &Location)
{
    if (Data.Data == nullptr || Data.Length == 0)
        return;

    // Use the internal Data pointer and Length from the Text class
    // We pass Types::Text to ensure the header is tagged correctly
    Set(Data.Data, Data.Length, Types::Text, Location);
}

template <>
void ByteArray::Set(const Reference &Data, const Path &Location)
{
    // Total size = 3 (Net, Group, Device) + Path.Length
    uint16_t totalSize = 3 + Data.Location.Length;

    // We need a temporary buffer to pack the 3-byte header with the path
    char *temp = new char[totalSize];

    temp[0] = (char)Data.Net;
    temp[1] = (char)Data.Group;
    temp[2] = (char)Data.Device;

    if (Data.Location.Length > 0 && Data.Location.Indexing != nullptr)
        memcpy(temp + 3, Data.Location.Indexing, Data.Location.Length);

    // Call the base Set. This handles Insert/Resize and updates the Header.Length
    Set(temp, totalSize, Types::Reference, Location);

    delete[] temp;
}