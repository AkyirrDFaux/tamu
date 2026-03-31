bool ExecuteCompare(ByteArray &Values, Reference Index, Operations Op)
{
    // 1. Locate the operation base
    SearchResult Base = Values.Find(Index);
    if (!Base.Value) return true;
    Bookmark OpPoint = Base.Location;

    // --- INPUT RESOLUTION ---
    // Dive to {0} (Output), then Step to {1} (Inputs Folder)
    SearchResult InputsFolder = Values.Child(OpPoint);
    InputsFolder = Values.Next(InputsFolder.Location);
    if (!InputsFolder.Value) return true;

    // Dive into Inputs to find {1, 0} (A) and Step to {1, 1} (B)
    SearchResult S_A = Values.Child(InputsFolder.Location);
    SearchResult S_B = Values.Next(S_A.Location);

    if (!S_A.Value || !S_B.Value) return true;

    // --- DATA VALIDATION ---
    // Strict Scalar Check: Reject if either input is not a scalar (Vector, etc.)
    if (!IsScalar(S_A.Location.Map->This(S_A.Location).Type) || 
        !IsScalar(S_B.Location.Map->This(S_B.Location).Type))
        return true;

    // --- EXECUTION ---
    // Promote both to Number for a unified comparison lane
    Number Left = GetAsNumber(S_A.Location);
    Number Right = GetAsNumber(S_B.Location);
    bool result = false;

    switch (Op)
    {
    case Operations::IsEqual:   result = (Left == Right); break;
    case Operations::IsGreater: result = (Left > Right);  break;
    case Operations::IsSmaller: result = (Left < Right);  break;
    default: return true;
    }

    // --- OUTPUT STORAGE ---
    // Output is always at relative {0}
    SearchResult S_Out = Values.Child(OpPoint);
    if (S_Out.Value)
    {
        uint8_t val = result ? 1 : 0;
        // Result of a comparison is always a 1-byte Boolean
        Values.SetDirect(&val, 1, Types::Bool, S_Out.Location);
    }

    return true;
}