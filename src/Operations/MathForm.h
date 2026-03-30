bool ExecuteSine(ByteArray &Values, Reference Index)
{
    // 1. Locate the operation base to get the output bookmark
    SearchResult Base = Values.Find(Index);
    if (!Base.Value) return true;
    
    // Parameter branch is at relative {1}
    SearchResult ParamBranch = Values.Find(Index.Append({1}), true);
    if (!ParamBranch.Value) return true;

    // 2. Linear traversal of parameters: {1, 0} Time, {1, 1} Mult, {1, 2} Phase
    SearchResult S_Time  = Values.Child(ParamBranch.Location);
    SearchResult S_Mult  = Values.Next(S_Time.Location);
    SearchResult S_Phase = Values.Next(S_Mult.Location);

    // 3. Resolve and Extract
    // Resolve Time (Expected Integer)
    SearchResult TimeActual = Values.This(S_Time.Location);
    if (TimeActual.Type != Types::Integer || !TimeActual.Value) return true;
    int32_t T = *(int32_t*)TimeActual.Value;

    // Resolve Mult and Phase (Expected Numbers)
    // We use This() to allow these to be references to other objects
    SearchResult MultActual  = Values.This(S_Mult.Location);
    SearchResult PhaseActual = Values.This(S_Phase.Location);

    Number M = (MultActual.Type == Types::Number && MultActual.Value) ? *(Number*)MultActual.Value : Number(1.0f);
    Number P = (PhaseActual.Type == Types::Number && PhaseActual.Value) ? *(Number*)PhaseActual.Value : Number(0.0f);

    // 4. Calculation
    Number Result = sin((Number)T * M + P);

    // 5. Optimized Store at relative {0}
    SearchResult S_Out = Values.Child(Base.Location);
    if (S_Out.Value) {
        Values.SetDirect(&Result, sizeof(Number), Types::Number, S_Out.Location);
    }

    return true;
}