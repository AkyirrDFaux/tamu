ByteArray BaseClass::Compress() const
{
    int32_t Index = Objects.Search(this);
    if (Index == -1)
        return ByteArray();

    uint8_t G = Objects.Object[Index].GroupID;
    uint8_t D = Objects.Object[Index].DeviceID;
    Reference SelfRef = Reference::Global(0, G, D);

    // Start the blob with the Reference type
    ByteArray Blob(&SelfRef, SelfRef.PathLen() + 4, Types::Reference);

    // Chain the rest of the object data
    Blob << ByteArray(&Type, sizeof(ObjectTypes), Types::ObjectType)
         << ByteArray(&Objects.Object[Index].Info, sizeof(ObjectInfo), Types::ObjectInfo)
         << ByteArray(Name.Data, Name.Length, Types::Text)
         << Values;

    return Blob;
}

SearchResult BaseClass::Find(const Reference &Location, bool StopAtReferences) const
{
    return Values.Find(Location, StopAtReferences);
}

void BaseClass::ValueSetup(const void *Data, size_t Size, Types Type, const Reference &Location)
{
    // Logic nodes calling MyObj.ValueSetup(...)
    Values.Set(Data, Size, Type, Location);
    Setup(Location);
}

void BaseClass::ValueSetupDirect(const void *Data, size_t Size, Types Type, const Bookmark &Point)
{
    // Logic nodes calling MyObj.ValueSetup with a cached Bookmark
    if (Point.Map)
        Point.Map->SetDirect(Data, Size, Type, Point);
}