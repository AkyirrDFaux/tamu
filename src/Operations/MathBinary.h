bool ExecuteCompare(const Bookmark &OpPoint, Operations Op)
{
    // 1. Navigation from OpPoint root
    // Relative {0} is the Output, {1} is the Inputs Folder
    Bookmark OutMark = OpPoint.Map->Child(OpPoint);
    if (OutMark.Index == INVALID_HEADER) return true;

    Bookmark InputsFolder = OpPoint.Map->Next(OutMark);
    if (InputsFolder.Index == INVALID_HEADER) return true;

    // Dive into Inputs folder to find {1, 0} (A) and Step to {1, 1} (B)
    Bookmark Mark_A = OpPoint.Map->Child(InputsFolder);
    if (Mark_A.Index == INVALID_HEADER) return true;

    Bookmark Mark_B = OpPoint.Map->Next(Mark_A);
    if (Mark_B.Index == INVALID_HEADER) return true;

    // --- DATA VALIDATION ---
    // Use the Map to resolve the Results for metadata checks
    Result Res_A = OpPoint.Map->GetThis(Mark_A);
    Result Res_B = OpPoint.Map->GetThis(Mark_B);

    if (!IsScalar(Res_A.Type) || !IsScalar(Res_B.Type))
        return true;

    // --- EXECUTION ---
    // GetAsNumber works directly with Bookmarks
    Number Left  = GetAsNumber(Mark_A);
    Number Right = GetAsNumber(Mark_B);
    bool result = false;

    switch (Op)
    {
    case Operations::IsEqual:   result = (Left == Right); break;
    case Operations::IsGreater: result = (Left > Right);  break;
    case Operations::IsSmaller: result = (Left < Right);  break;
    default: return true;
    }

    // --- OUTPUT STORAGE ---
    // Direct set using the Output node's index
    OpPoint.Map->Set(&result, sizeof(bool), Types::Bool, OutMark.Index);

    return true;
}