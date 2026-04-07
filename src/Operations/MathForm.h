bool ExecuteSine(const Bookmark &OpPoint)
{
    // 1. Navigation: {0} is Output, {1} is Time, {2} is Mult, {3} is Phase
    Bookmark OutMark = OpPoint.Child();
    if (OutMark.Index == INVALID_HEADER)
        return true;

    Bookmark markTime = OutMark.Next();
    Bookmark markMult = markTime.Next();
    Bookmark markPhase = markMult.Next();

    if (markTime.Index == INVALID_HEADER || markMult.Index == INVALID_HEADER || markPhase.Index == INVALID_HEADER)
        return true;

    // 2. Calculation
    Number T = GetAsNumber(markTime);
    Number M = GetAsNumber(markMult);
    Number P = GetAsNumber(markPhase);

    Number ResultValue = sin(T * M + P);

    // 3. Store result
    OutMark.Set(&ResultValue, sizeof(Number), Types::Number, true);

    return true;
}

bool ExecuteClamp(const Bookmark &OpPoint)
{
    // 1. Navigation
    Bookmark OutMark = OpPoint.Child();
    if (OutMark.Index == INVALID_HEADER) return true;

    Bookmark markVal = OutMark.Next(); 
    Bookmark markMin = markVal.Next(); 
    Bookmark markMax = markMin.Next(); 

    if (markVal.Index == INVALID_HEADER || markMin.Index == INVALID_HEADER || markMax.Index == INVALID_HEADER)
        return true;

    // 2. Resolve for Type Persistence
    Result resVal = markVal.GetThis();
    if (!IsScalar(resVal.Type)) return true;

    Number V = GetAsNumber(markVal);
    Number Low = GetAsNumber(markMin);
    Number High = GetAsNumber(markMax);

    if (V < Low) V = Low;
    if (V > High) V = High;

    // 3. Store back using original type
    StoreScalar(OutMark, V, resVal.Type);
    return true;
}

bool ExecuteLerp(const Bookmark &OpPoint)
{
    // 1. Navigation
    Bookmark OutMark = OpPoint.Child();
    if (OutMark.Index == INVALID_HEADER) return true;

    Bookmark markLow   = OutMark.Next(); 
    Bookmark markHigh  = markLow.Next();  
    Bookmark markAlpha = markHigh.Next(); 
    Bookmark markScale = markAlpha.Next(); 

    if (markLow.Index == INVALID_HEADER || markHigh.Index == INVALID_HEADER || markAlpha.Index == INVALID_HEADER)
        return true;

    Result resLow = markLow.GetThis();
    if (!IsScalar(resLow.Type)) return true;

    Number A = GetAsNumber(markLow);
    Number B = GetAsNumber(markHigh);
    Number T = GetAsNumber(markAlpha);

    Number Scale = 1.0f;
    if (markScale.Index != INVALID_HEADER)
    {
        Scale = GetAsNumber(markScale);
        if (Scale == Number(0.0f)) Scale = 1.0f;
    }

    Number ResultValue = A + (T / Scale) * (B - A);

    // 2. Store result
    StoreScalar(OutMark, ResultValue, resLow.Type);
    return true;
}