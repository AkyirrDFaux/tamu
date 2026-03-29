bool Sine(ByteArray &Values, Reference Index)
{
    Reference OutPath = Index.Append(0);
    
    // 1. Get the Bookmark for the input branch {Index, 1}
    SearchResult BranchRes = Values.Find(Index.Append(1));
    if (BranchRes.Length == 0) return true;
    
    Bookmark InBranch = BranchRes.Location;

    // 2. Fetch inputs relative to the bookmark
    // We expect: {1, 0} = Time (Int), {1, 1} = Multiplier (Number), {1, 2} = Phase (Number)
    SearchResult S_Time  = Values.Find(InBranch, Reference({0}));
    SearchResult S_Mult  = Values.Find(InBranch, Reference({1}));
    SearchResult S_Phase = Values.Find(InBranch, Reference({2}));

    // 3. Validation and Extraction
    // Time is usually an Integer (system ticks)
    if (S_Time.Type != Types::Integer || !S_Time.Value) return true;
    int32_t T = *(int32_t*)S_Time.Value;

    // Mult and Phase are usually Numbers (floats)
    // We provide defaults (1.0 and 0.0) if they are missing or wrong type
    Number M = (S_Mult.Type == Types::Number && S_Mult.Value) ? *(Number*)S_Mult.Value : Number(1.0f);
    Number P = (S_Phase.Type == Types::Number && S_Phase.Value) ? *(Number*)S_Phase.Value : Number(0.0f);

    // 4. Calculation
    // Using standard sinf for performance on embedded (assuming Number is float)
    Number Result = sin((Number)T * M + P);

    // 5. Store Result
    Values.Set(&Result, sizeof(Number), Types::Number, OutPath);

    return true;
}