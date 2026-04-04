FlexArray BaseClass::Compress() const
{
    // 1. Get the Reference struct/object
    Reference SelfRef = Objects.GetReference(this);

    // 2. Start the FlexArray with exactly 3 bytes from the Reference data
    // This assumes Reference.Data is the internal pointer to [Net, Group, Device]
    FlexArray Blob((const char *)SelfRef.Data, 3);

    // 3. Append ObjectType + Flags + Info (4 byte consecutive)
    Blob += FlexArray((char*)&Type, 4);

    // 5. Append Name (1-byte length prefix + data)
    uint8_t nameLen = (Name.Length > 255) ? 255 : (uint8_t)Name.Length-1;
    Blob += FlexArray((char *)&nameLen, 1);
    if (nameLen > 0)
    {
        Blob += FlexArray(Name.Data, nameLen);
    }

    // 6. Serialize and Append Values
    // This calls the loop-based serialization we discussed
    Blob += Values.Serialize();

    return Blob;
}

Bookmark BaseClass::Find(const Reference &Location, bool StopAtReferences) const
{
    return Values.Find(Location, StopAtReferences);
}

void BaseClass::ValueSetup(const void *Data, size_t Size, Types Type, const Reference &Location)
{
    // Logic nodes calling MyObj.ValueSetup(...)
    Values.Set(Data, Size, Type, Location);
    Setup(Location);
}

void BaseClass::ValueSetup(const void *Data, size_t Size, Types Type, const Bookmark &Point)
{
    // Logic nodes calling MyObj.ValueSetup with a cached Bookmark
    if (Point.Map)
        Point.Map->Set(Data, Size, Type, Point);
}