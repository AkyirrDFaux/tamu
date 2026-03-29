bool ExecuteCompare(ByteArray &Values, Reference Index, Operations Op)
{
    // Path: [Output (Byte), InputA, InputB]
    Reference OutPath = Index.Append(0);
    Reference InAPath = Index.Append(1);
    Reference InBPath = Index.Append(2);

    Types TypeA = Values.Type(InAPath);
    Types TypeB = Values.Type(InBPath);

    // 1. Strict Scalar Check: If either is a Vector, bail.
    if (TypeA == Types::Vector2D || TypeA == Types::Vector3D || TypeB == Types::Vector2D || TypeB == Types::Vector3D)
        return true;

    // 2. Use our existing Merger for Scalar upcasting (Byte -> Int -> Number)
    Types CompareType = MergeTypes(TypeA, TypeB);
    if (CompareType == Types::Undefined)
        return true;

    // 3. Fetch and Promote
    Arithmetic Left = Fetch(Values, InAPath, CompareType);
    Arithmetic Right = Fetch(Values, InBPath, CompareType);

    bool result = false;

    // 4. Concrete Scalar Comparison
    switch (Op)
    {
    case Operations::IsEqual:
        if (CompareType == Types::Number)
            result = (Left.Value.n == Right.Value.n);
        else if (CompareType == Types::Integer)
            result = (Left.Value.i == Right.Value.i);
        else
            result = (Left.Value.b == Right.Value.b);
        break;

    case Operations::IsGreater:
        if (CompareType == Types::Number)
            result = (Left.Value.n > Right.Value.n);
        else if (CompareType == Types::Integer)
            result = (Left.Value.i > Right.Value.i);
        else
            result = (Left.Value.b > Right.Value.b);
        break;

    case Operations::IsSmaller:
        if (CompareType == Types::Number)
            result = (Left.Value.n < Right.Value.n);
        else if (CompareType == Types::Integer)
            result = (Left.Value.i < Right.Value.i);
        else
            result = (Left.Value.b < Right.Value.b);
        break;

    default:
        return true;
    }

    // Store result as a Boolean Byte (1 or 0)
    Values.Set((uint8_t)(result ? 1 : 0), OutPath);
    return true;
}