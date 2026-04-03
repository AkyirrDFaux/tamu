Number GetAsNumber(const Bookmark &Location)
{
    // 1. Resolve the data (Handles POD and Reference teleportation)
    // We use the Map stored in the bookmark to ensure context-safety
    SearchResult Source = Location.Map->This(Location);

    // 2. If resolution fails or it's not a scalar-compatible type, return 0
    if (!Source.Value || Source.Type == Types::Undefined)
        return 0.0f;

    // 3. Strict Scalar Extraction with promotion to 'Number'
    switch (Source.Type)
    {
    case Types::Number:
        return *(Number *)Source.Value;
    case Types::Integer:
        return (Number)(*(int32_t *)Source.Value);
    case Types::Byte:
    case Types::Bool:
        return (Number)(*(uint8_t *)Source.Value);

    // Vectors are treated separately (not scalars), so they return 0 here
    default:
        return 0.0f;
    }
}

void StoreScalar(ValueTree &Values, Reference Path, Number Result, Types T)
{
    size_t sz = sizeof(Number);
    if (T == Types::Integer)
        sz = sizeof(int32_t);
    else if (T == Types::Byte || T == Types::Bool)
        sz = 1;
    Values.Set(&Result, sz, T, Path);
}