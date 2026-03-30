bool ExecuteCompare(ByteArray &Values, Reference Index, Operations Op)
{
    // 1. Locate the operation base
    SearchResult Base = Values.Find(Index);
    if (!Base.Value) return true;
    Bookmark OpPoint = Base.Location;

    // --- INPUT RESOLUTION ---
    // Goal: Parameter A is at Index + {1, 0}
    // Goal: Parameter B is at Index + {1, 1}

    // Step 1: Move to the 'Inputs' folder at relative {1}
    SearchResult InputsFolder = Values.Child(OpPoint);      // Move to {0} (Output)
    InputsFolder = Values.Next(InputsFolder.Location);      // Move to {1} (Inputs Folder)
    if (!InputsFolder.Value) return true;

    // Step 2: Dive into the Inputs folder to find the first parameter {1, 0}
    SearchResult S_A = Values.Child(InputsFolder.Location); // Move to {1, 0}
    
    // Step 3: Move to the second parameter {1, 1}
    SearchResult S_B = Values.Next(S_A.Location);           // Move to {1, 1}

    if (!S_A.Value || !S_B.Value) return true;

    // --- DATA RESOLUTION ---
    // 3. Resolve Reference Types to determine actual POD data
    // Use the map from the bookmark to handle potential global references
    SearchResult A_Actual = S_A.Location.Map->This(S_A.Location);
    SearchResult B_Actual = S_B.Location.Map->This(S_B.Location);

    // Strict Scalar Check: No Vectors allowed for these comparison ops
    if (A_Actual.Type == Types::Vector2D || A_Actual.Type == Types::Vector3D ||
        B_Actual.Type == Types::Vector2D || B_Actual.Type == Types::Vector3D)
        return true;

    // 4. Determine Comparison Type (Upcasting)
    Types CompareType = MergeTypes(A_Actual.Type, B_Actual.Type);
    if (CompareType == Types::Undefined) return true;

    // 5. Fetch and Promote (Pass the specific Map from the Bookmark)
    Arithmetic Left = Fetch(*S_A.Location.Map, S_A.Location, CompareType);
    Arithmetic Right = Fetch(*S_B.Location.Map, S_B.Location, CompareType);

    bool result = false;

    // 6. Concrete Scalar Comparison
    switch (Op)
    {
    case Operations::IsEqual:
        if (CompareType == Types::Number) result = (Left.Value.n == Right.Value.n);
        else if (CompareType == Types::Integer) result = (Left.Value.i == Right.Value.i);
        else result = (Left.Value.b == Right.Value.b);
        break;

    case Operations::IsGreater:
        if (CompareType == Types::Number) result = (Left.Value.n > Right.Value.n);
        else if (CompareType == Types::Integer) result = (Left.Value.i > Right.Value.i);
        else result = (Left.Value.b > Right.Value.b);
        break;

    case Operations::IsSmaller:
        if (CompareType == Types::Number) result = (Left.Value.n < Right.Value.n);
        else if (CompareType == Types::Integer) result = (Left.Value.i < Right.Value.i);
        else result = (Left.Value.b < Right.Value.b);
        break;

    default: return true;
    }

    // --- OUTPUT STORAGE ---
    // 7. Store result as a Boolean Byte at relative {0}
    SearchResult S_Out = Values.Child(OpPoint); // Output is always at {0}
    if (S_Out.Value)
    {
        uint8_t val = result ? 1 : 0;
        // Use SetDirect to write to the specifically located Bookmark
        Values.SetDirect(&val, 1, Types::Bool, S_Out.Location);
    }

    return true;
}