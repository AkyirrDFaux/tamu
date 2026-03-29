Types MergeTypes(Types Current, Types Next)
{
    if (Current == Types::Undefined || Next == Types::Undefined)
        return Types::Undefined;
    if (Current == Next)
        return Current;

    // Determine the broader type (Upcasting Only)
    // Rule: Scalars (Byte < Int < Number) can promote to each other or to Vectors.
    // Rule: Vectors cannot promote to each other.

    // Handle Vector3D
    if (Current == Types::Vector3D || Next == Types::Vector3D)
    {
        Types Other = (Current == Types::Vector3D) ? Next : Current;
        // Only scalars can merge into Vector3D
        if (Other == Types::Number || Other == Types::Integer || Other == Types::Byte)
            return Types::Vector3D;
        return Types::Undefined; // Vector2D vs Vector3D = Illegal
    }

    // Handle Vector2D
    if (Current == Types::Vector2D || Next == Types::Vector2D)
    {
        Types Other = (Current == Types::Vector2D) ? Next : Current;
        if (Other == Types::Number || Other == Types::Integer || Other == Types::Byte)
            return Types::Vector2D;
        return Types::Undefined;
    }

    // Handle Scalars
    if (Current == Types::Number || Next == Types::Number)
        return Types::Number;
    if (Current == Types::Integer || Next == Types::Integer)
        return Types::Integer;

    return Types::Byte;
}

struct Arithmetic
{
    Types Type = Types::Undefined;

    // Union to save space, but Vector3D is the "ceiling"
    union ValueUnion
    {
        Number n;
        int32_t i;
        uint8_t b;
        Vector2D v2;
        Vector3D v3;
        ValueUnion() {}
    } Value;

    // The math router
    void Apply(const Arithmetic &Next, Operations Op);
};

bool StoreArithmetic(ByteArray &Values, Reference Path, const Arithmetic &Data)
{
    if (Data.Type == Types::Undefined)
        return false;

    switch (Data.Type)
    {
    case Types::Vector3D:
        Values.Set(Data.Value.v3, Path);
        return true;
    case Types::Vector2D:
        Values.Set(Data.Value.v2, Path);
        return true;
    case Types::Number:
        Values.Set(Data.Value.n, Path);
        return true;
    case Types::Integer:
        Values.Set(Data.Value.i, Path);
        return true;
    case Types::Byte:
        Values.Set(Data.Value.b, Path);
        return true;
    default:
        return false;
    }
}

Arithmetic Fetch(ByteArray &Values, Reference Path, Types TargetType)
{
    Arithmetic acc;
    acc.Type = TargetType;
    Types SourceType = Values.Type(Path);

    if (TargetType == Types::Vector3D)
    {
        if (SourceType == Types::Vector3D)
            acc.Value.v3 = Values.Get<Vector3D>(Path).Value;
        // Vector2D to Vector3D is "Lossless" (Z=0), but you mentioned they
        // aren't comparable, so we'll block it to be safe.
        else if (SourceType == Types::Number || SourceType == Types::Integer || SourceType == Types::Byte)
        {
            Number n = (SourceType == Types::Number) ? Values.Get<Number>(Path).Value : (SourceType == Types::Integer) ? (Number)Values.Get<int32_t>(Path).Value
                                                                                                                       : (Number)Values.Get<uint8_t>(Path).Value;
            acc.Value.v3 = {n, n, n};
        }
        else
            acc.Type = Types::Undefined;
    }
    else if (TargetType == Types::Vector2D)
    {
        if (SourceType == Types::Vector2D)
            acc.Value.v2 = Values.Get<Vector2D>(Path).Value;
        else if (SourceType == Types::Number || SourceType == Types::Integer || SourceType == Types::Byte)
        {
            Number n = (SourceType == Types::Number) ? Values.Get<Number>(Path).Value : (SourceType == Types::Integer) ? (Number)Values.Get<int32_t>(Path).Value
                                                                                                                       : (Number)Values.Get<uint8_t>(Path).Value;
            acc.Value.v2 = {n, n};
        }
        else
            acc.Type = Types::Undefined;
    }
    else if (TargetType == Types::Number)
    {
        if (SourceType == Types::Number)
            acc.Value.n = Values.Get<Number>(Path).Value;
        else if (SourceType == Types::Integer)
            acc.Value.n = (Number)Values.Get<int32_t>(Path).Value;
        else if (SourceType == Types::Byte)
            acc.Value.n = (Number)Values.Get<uint8_t>(Path).Value;
        else
            acc.Type = Types::Undefined;
    }
    else if (TargetType == Types::Integer)
    {
        if (SourceType == Types::Integer)
            acc.Value.i = Values.Get<int32_t>(Path).Value;
        else if (SourceType == Types::Byte)
            acc.Value.i = (int32_t)Values.Get<uint8_t>(Path).Value;
        else
            acc.Type = Types::Undefined; // Block Number -> Integer
    }
    else if (TargetType == Types::Byte)
    {
        if (SourceType == Types::Byte)
            acc.Value.b = Values.Get<uint8_t>(Path).Value;
        else
            acc.Type = Types::Undefined; // Block all others
    }

    return acc;
}