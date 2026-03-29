Types MergeTypes(Types Current, Types Next)
{
    if (Current == Next) return Current;
    if (Current == Types::Undefined || Next == Types::Undefined) return Types::Undefined;

    // 1. Vector3D Promotion
    if (Current == Types::Vector3D || Next == Types::Vector3D) {
        Types Other = (Current == Types::Vector3D) ? Next : Current;
        if (Other == Types::Vector3D || (Other == Types::Number || Other == Types::Integer || Other == Types::Byte))
            return Types::Vector3D;
        return Types::Undefined;
    }

    // 2. Vector2D Promotion
    if (Current == Types::Vector2D || Next == Types::Vector2D) {
        Types Other = (Current == Types::Vector2D) ? Next : Current;
        if (Other == Types::Vector2D || (Other == Types::Number || Other == Types::Integer || Other == Types::Byte))
            return Types::Vector2D;
        return Types::Undefined;
    }

    // 3. Scalar Promotion (Number > Integer > Byte/Bool)
    if (Current == Types::Number || Next == Types::Number) return Types::Number;
    if (Current == Types::Integer || Next == Types::Integer) return Types::Integer;
    
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
    if (Data.Type == Types::Undefined) return false;

    switch (Data.Type)
    {
        case Types::Vector3D:
            Values.Set(&Data.Value.v3, sizeof(Vector3D), Types::Vector3D, Path);
            return true;
        case Types::Vector2D:
            Values.Set(&Data.Value.v2, sizeof(Vector2D), Types::Vector2D, Path);
            return true;
        case Types::Number:
            Values.Set(&Data.Value.n, sizeof(Number), Types::Number, Path);
            return true;
        case Types::Integer:
            Values.Set(&Data.Value.i, sizeof(int32_t), Types::Integer, Path);
            return true;
        case Types::Byte:
            Values.Set(&Data.Value.b, sizeof(uint8_t), Types::Byte, Path);
            return true;
        default:
            return false;
    }
}

Arithmetic Fetch(ByteArray &Values, Reference Path, Types TargetType)
{
    Arithmetic acc;
    acc.Type = TargetType;

    // 1. Single walk to find the data
    SearchResult Source = Values.Find(Path);

    // If data doesn't exist or is invalid
    if (!Source.Value || Source.Length == 0)
    {
        acc.Type = Types::Undefined;
        return acc;
    }

    // Manual scalar extraction (replacing the lambda/auto)
    Number sourceAsNumber = 0.0f;
    bool isScalar = false;

    if (Source.Type == Types::Number) {
        sourceAsNumber = *(Number*)Source.Value;
        isScalar = true;
    } else if (Source.Type == Types::Integer) {
        sourceAsNumber = (Number)(*(int32_t*)Source.Value);
        isScalar = true;
    } else if (Source.Type == Types::Byte) {
        sourceAsNumber = (Number)(*(uint8_t*)Source.Value);
        isScalar = true;
    }

    // 2. Target Mapping Logic
    if (TargetType == Types::Vector3D)
    {
        if (Source.Type == Types::Vector3D) {
            acc.Value.v3 = *(Vector3D*)Source.Value;
        } else if (isScalar) {
            acc.Value.v3 = Vector3D(sourceAsNumber, sourceAsNumber, sourceAsNumber);
        } else {
            acc.Type = Types::Undefined;
        }
    }
    else if (TargetType == Types::Vector2D)
    {
        if (Source.Type == Types::Vector2D) {
            acc.Value.v2 = *(Vector2D*)Source.Value;
        } else if (isScalar) {
            acc.Value.v2 = Vector2D(sourceAsNumber, sourceAsNumber);
        } else {
            acc.Type = Types::Undefined;
        }
    }
    else if (TargetType == Types::Number)
    {
        if (isScalar) {
            acc.Value.n = sourceAsNumber;
        } else {
            acc.Type = Types::Undefined;
        }
    }
    else if (TargetType == Types::Integer)
    {
        if (Source.Type == Types::Integer) {
            acc.Value.i = *(int32_t*)Source.Value;
        } else if (Source.Type == Types::Byte) {
            acc.Value.i = (int32_t)(*(uint8_t*)Source.Value);
        } else {
            acc.Type = Types::Undefined;
        }
    }
    else if (TargetType == Types::Byte)
    {
        if (Source.Type == Types::Byte) {
            acc.Value.b = *(uint8_t*)Source.Value;
        } else {
            acc.Type = Types::Undefined;
        }
    }

    return acc;
}

Arithmetic Fetch(ByteArray &Values, const Bookmark &Parent, Reference RelativePath, Types TargetType)
{
    Arithmetic acc;
    acc.Type = TargetType;

    // 1. Single walk relative to the Parent Bookmark
    // Our new Find handles the Dive into Parent automatically
    SearchResult Source = Values.Find(Parent, RelativePath);

    // If data doesn't exist or is invalid
    if (!Source.Value || Source.Length == 0)
    {
        acc.Type = Types::Undefined;
        return acc;
    }

    // Manual scalar extraction
    Number sourceAsNumber = 0.0f;
    bool isScalar = false;

    if (Source.Type == Types::Number) {
        sourceAsNumber = *(Number*)Source.Value;
        isScalar = true;
    } else if (Source.Type == Types::Integer) {
        sourceAsNumber = (Number)(*(int32_t*)Source.Value);
        isScalar = true;
    } else if (Source.Type == Types::Byte || Source.Type == Types::Bool) {
        sourceAsNumber = (Number)(*(uint8_t*)Source.Value);
        isScalar = true;
    }

    // 2. Target Mapping Logic
    if (TargetType == Types::Vector3D)
    {
        if (Source.Type == Types::Vector3D) {
            acc.Value.v3 = *(Vector3D*)Source.Value;
        } else if (isScalar) {
            acc.Value.v3 = Vector3D(sourceAsNumber, sourceAsNumber, sourceAsNumber);
        } else {
            acc.Type = Types::Undefined;
        }
    }
    else if (TargetType == Types::Vector2D)
    {
        if (Source.Type == Types::Vector2D) {
            acc.Value.v2 = *(Vector2D*)Source.Value;
        } else if (isScalar) {
            acc.Value.v2 = Vector2D(sourceAsNumber, sourceAsNumber);
        } else {
            acc.Type = Types::Undefined;
        }
    }
    else if (TargetType == Types::Number)
    {
        if (isScalar) {
            acc.Value.n = sourceAsNumber;
        } else {
            acc.Type = Types::Undefined;
        }
    }
    else if (TargetType == Types::Integer)
    {
        if (Source.Type == Types::Integer) {
            acc.Value.i = *(int32_t*)Source.Value;
        } else if (Source.Type == Types::Byte || Source.Type == Types::Bool) {
            acc.Value.i = (int32_t)(*(uint8_t*)Source.Value);
        } else if (Source.Type == Types::Number) {
            acc.Value.i = (int32_t)(*(Number*)Source.Value);
        } else {
            acc.Type = Types::Undefined;
        }
    }
    else if (TargetType == Types::Byte || TargetType == Types::Bool)
    {
        if (Source.Type == Types::Byte || Source.Type == Types::Bool) {
            acc.Value.b = *(uint8_t*)Source.Value;
        } else if (isScalar) {
            acc.Value.b = (uint8_t)sourceAsNumber;
        } else {
            acc.Type = Types::Undefined;
        }
    }

    return acc;
}