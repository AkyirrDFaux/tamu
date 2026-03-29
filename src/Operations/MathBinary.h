bool ExecuteCompare(ByteArray &Values, Reference Index, Operations Op)
{
    // Path: [Output (0), InputA (1), InputB (2)]
    // 1. Get the base bookmark for this operation to speed up child lookups
    SearchResult Base = Values.Find(Index);
    if (Base.Length == 0)
        return true;
    Bookmark OpPoint = Base.Location;

    // 2. Locate Inputs relative to the operation bookmark
    SearchResult S_A = Values.Find(OpPoint, Reference({1}));
    SearchResult S_B = Values.Find(OpPoint, Reference({2}));

    if (!S_A.Value || !S_B.Value)
        return true;

    // 3. Strict Scalar Check: No Vectors allowed in standard comparison
    if (S_A.Type == Types::Vector2D || S_A.Type == Types::Vector3D ||
        S_B.Type == Types::Vector2D || S_B.Type == Types::Vector3D)
        return true;

    // 4. Determine Comparison Type (Upcasting)
    Types CompareType = MergeTypes(S_A.Type, S_B.Type);
    if (CompareType == Types::Undefined)
        return true;

    // 5. Fetch and Promote using our optimized Fetch
    // We pass the global path here because Fetch handles the search logic internally
    Arithmetic Left = Fetch(Values, Index.Append(1), CompareType);
    Arithmetic Right = Fetch(Values, Index.Append(2), CompareType);

    bool result = false;

    // 6. Concrete Scalar Comparison
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

    // 7. Store result as a Boolean Byte (1 or 0)
    // We use Reference(0) relative to OpPoint for the output
    Values.Set(&result, sizeof(bool), Types::Bool, Index.Append(0));

    return true;
}