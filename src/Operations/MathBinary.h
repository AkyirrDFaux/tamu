bool ExecuteCompare(const Bookmark &OpPoint, Operations Op)
{
    // 1. Navigation from OpPoint root
    // Relative {0} is the Output, {1} is the Inputs Folder
    Bookmark OutMark = OpPoint.Child();
    if (OutMark.Index == INVALID_HEADER)
        return true;

    Bookmark InputsFolder = OutMark.Next();
    if (InputsFolder.Index == INVALID_HEADER)
        return true;

    // Dive into Inputs folder to find {1, 0} (A) and Step to {1, 1} (B)
    Bookmark Mark_A = InputsFolder.Child();
    if (Mark_A.Index == INVALID_HEADER)
        return true;

    Bookmark Mark_B = Mark_A.Next();
    if (Mark_B.Index == INVALID_HEADER)
        return true;

    // --- DATA VALIDATION ---
    // Use the Map to resolve the Results for metadata checks
    Result Res_A = Mark_A.GetThis();
    Result Res_B = Mark_B.GetThis();

    if (!IsScalar(Res_A.Type) || !IsScalar(Res_B.Type))
        return true;

    // --- EXECUTION ---
    // GetAsNumber works directly with Bookmarks
    Number Left = GetAsNumber(Mark_A);
    Number Right = GetAsNumber(Mark_B);
    bool result = false;

    switch (Op)
    {
    case Operations::IsEqual:
        result = (Left == Right);
        break;
    case Operations::IsGreater:
        result = (Left > Right);
        break;
    case Operations::IsSmaller:
        result = (Left < Right);
        break;
    default:
        return true;
    }

    // --- OUTPUT STORAGE ---
    // Direct set using the Output node's index
    OutMark.Set(&result, sizeof(bool), Types::Bool, true);

    return true;
}