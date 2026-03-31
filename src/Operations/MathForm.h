bool ExecuteSine(ByteArray &Values, Reference Index)
{
    // 1. Locate the Inputs folder {1, 0}
    SearchResult it = Values.Find(Index.Append(1).Append(0), true);
    if (!it.Value) return true;

    // 2. Extract Parameters
    SearchResult S_Time  = it;
    SearchResult S_Mult  = Values.Next(S_Time.Location);
    SearchResult S_Phase = Values.Next(S_Mult.Location);

    if (!S_Time.Value || !S_Mult.Value || !S_Phase.Value) return true;

    // 3. Calculation
    // GetAsNumber handles reference resolution and promotes any scalar to Number
    Number T = GetAsNumber(S_Time.Location);
    Number M = GetAsNumber(S_Mult.Location);
    Number P = GetAsNumber(S_Phase.Location);
    
    Number Result = sin(T * M + P);

    // 4. Store result at relative {0}
    // Sine results are always stored as the full Number type
    Values.Set(&Result, sizeof(Number), Types::Number, Index.Append(0));
    
    return true;
}

bool ExecuteClamp(ByteArray &Values, Reference Index)
{
    SearchResult it = Values.Find(Index.Append(1).Append(0), true);
    if (!it.Value) return true;

    SearchResult S_Val = it;
    SearchResult S_Min = Values.Next(S_Val.Location);
    SearchResult S_Max = Values.Next(S_Min.Location);

    if (!S_Val.Value || !S_Min.Value || !S_Max.Value) return true;

    Types targetType = Values.This(S_Val.Location).Type;

    // Strict Scalar Check
    if (!IsScalar(targetType) || 
        !IsScalar(Values.This(S_Min.Location).Type) || 
        !IsScalar(Values.This(S_Max.Location).Type))
        return true;

    Number V = GetAsNumber(S_Val.Location);
    Number Low = GetAsNumber(S_Min.Location);
    Number High = GetAsNumber(S_Max.Location);

    if (V < Low) V = Low;
    if (V > High) V = High;

    StoreScalar(Values, Index.Append(0), V, targetType);
    return true;
}

bool ExecuteLerp(ByteArray &Values, Reference Index)
{
    SearchResult it = Values.Find(Index.Append(1).Append(0), true);
    if (!it.Value) return true;

    SearchResult S_Low   = it;
    SearchResult S_High  = Values.Next(S_Low.Location);
    SearchResult S_Alpha = Values.Next(S_High.Location);
    SearchResult S_Scale = Values.Next(S_Alpha.Location);

    if (!S_Low.Value || !S_High.Value || !S_Alpha.Value) return true;

    Types targetType = Values.This(S_Low.Location).Type;

    // Reject if Low or High are not scalars
    if (!IsScalar(targetType) || !IsScalar(Values.This(S_High.Location).Type))
        return true;

    Number A = GetAsNumber(S_Low.Location);
    Number B = GetAsNumber(S_High.Location);
    Number T = GetAsNumber(S_Alpha.Location);
    
    Number Scale = 1.0f;
    if (S_Scale.Value)
    {
        Scale = GetAsNumber(S_Scale.Location);
        if (Scale == Number(0.0f)) Scale = 1.0f;
    }

    Number Result = A + (T / Scale) * (B - A);

    StoreScalar(Values, Index.Append(0), Result, targetType);
    return true;
}