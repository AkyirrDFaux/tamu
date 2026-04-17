Number GetAsNumber(const Bookmark &Location)
{
    // 1. Guard against invalid bookmarks
    if (Location.Index == INVALID_HEADER)
        return N(0.0);

    // 2. Resolve the data Result via the Map stored in the bookmark
    // This handles the raw memory access and type metadata
    Result Source = Location.GetThis();

    // 3. If resolution fails or data is missing, return 0
    if (!Source.Value || Source.Type == Types::Undefined)
        return N(0.0);

    // 4. Scalar Extraction with promotion to 'Number'
    switch (Source.Type)
    {
    case Types::Number:
        return *(Number *)Source.Value;

    case Types::Integer:
        // Promote 32-bit signed integer to floating-point Number
        return (Number)(*(int32_t *)Source.Value);

    case Types::Byte:
    case Types::Bool:
        // Promote 8-bit unsigned types to floating-point Number
        return (Number)(*(uint8_t *)Source.Value);

    // Complex types (Vectors, Colors, etc.) are not scalars, return 0
    default:
        return N(0.0);
    }
}

void StoreScalar(const Bookmark &Target, Number Result, Types T)
{
    // 1. Safety check for the bookmark index
    if (Target.Index == INVALID_HEADER)
        return;

    // 2. Branching and Casting based on the target Type
    if (T == Types::Integer)
    {
        int32_t val = Result.RoundToInt();
        Target.SetCurrent(&val, sizeof(int32_t), T);
    }
    else if (T == Types::Byte || T == Types::Bool)
    {
        uint8_t val = Result.RoundToInt();
        Target.SetCurrent(&val, 1, T);
    }
    else if (T == Types::Number)
    {
        // Standard Number type (usually float or double)
        Target.SetCurrent(&Result, sizeof(Number), T);
    }
}