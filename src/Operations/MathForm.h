bool ExecuteSine(const Bookmark &OpPoint)
{
    // 1. Navigation: {0} is Output, {1} is Inputs folder, {1, 0} is first input
    Bookmark OutMark = OpPoint.Map->Child(OpPoint);
    Bookmark InFolder = OpPoint.Map->Next(OutMark);
    if (InFolder.Index == INVALID_HEADER) return true;

    Bookmark markTime = OpPoint.Map->Child(InFolder);
    if (markTime.Index == INVALID_HEADER) return true;

    // 2. Extract Parameters using sibling traversal
    Bookmark markMult  = OpPoint.Map->Next(markTime);
    Bookmark markPhase = OpPoint.Map->Next(markMult);

    if (markMult.Index == INVALID_HEADER || markPhase.Index == INVALID_HEADER)
        return true;

    // 3. Calculation
    Number T = GetAsNumber(markTime);
    Number M = GetAsNumber(markMult);
    Number P = GetAsNumber(markPhase);

    Number Result = sin(T * M + P);

    // 4. Store result at relative {0} using the Output index
    OpPoint.Map->Set(&Result, sizeof(Number), Types::Number, OutMark.Index);

    return true;
}

bool ExecuteClamp(const Bookmark &OpPoint)
{
    // 1. Navigation
    Bookmark OutMark = OpPoint.Map->Child(OpPoint);
    Bookmark InFolder = OpPoint.Map->Next(OutMark);
    if (InFolder.Index == INVALID_HEADER) return true;

    Bookmark markVal = OpPoint.Map->Child(InFolder);
    if (markVal.Index == INVALID_HEADER) return true;

    Bookmark markMin = OpPoint.Map->Next(markVal);
    Bookmark markMax = OpPoint.Map->Next(markMin);

    if (markMin.Index == INVALID_HEADER || markMax.Index == INVALID_HEADER)
        return true;

    // 2. Resolve Results for type checking
    Result resVal = OpPoint.Map->GetThis(markVal);
    Result resMin = OpPoint.Map->GetThis(markMin);
    Result resMax = OpPoint.Map->GetThis(markMax);

    // Strict Scalar Check
    if (!IsScalar(resVal.Type) || !IsScalar(resMin.Type) || !IsScalar(resMax.Type))
        return true;

    Number V = GetAsNumber(markVal);
    Number Low = GetAsNumber(markMin);
    Number High = GetAsNumber(markMax);

    if (V < Low) V = Low;
    if (V > High) V = High;

    // 3. Store back using Output node index and the original input type
    StoreScalar(OutMark, V, resVal.Type);
    return true;
}

bool ExecuteLerp(const Bookmark &OpPoint)
{
    // 1. Navigation
    Bookmark OutMark = OpPoint.Map->Child(OpPoint);
    Bookmark InFolder = OpPoint.Map->Next(OutMark);
    if (InFolder.Index == INVALID_HEADER) return true;

    Bookmark markLow = OpPoint.Map->Child(InFolder);
    if (markLow.Index == INVALID_HEADER) return true;

    Bookmark markHigh  = OpPoint.Map->Next(markLow);
    Bookmark markAlpha = OpPoint.Map->Next(markHigh);
    Bookmark markScale = OpPoint.Map->Next(markAlpha);

    if (markHigh.Index == INVALID_HEADER || markAlpha.Index == INVALID_HEADER)
        return true;

    Result resLow  = OpPoint.Map->GetThis(markLow);
    Result resHigh = OpPoint.Map->GetThis(markHigh);

    // Reject if Low or High are not scalars
    if (!IsScalar(resLow.Type) || !IsScalar(resHigh.Type))
        return true;

    Number A = GetAsNumber(markLow);
    Number B = GetAsNumber(markHigh);
    Number T = GetAsNumber(markAlpha);

    Number Scale = 1.0f;
    if (markScale.Index != INVALID_HEADER)
    {
        Scale = GetAsNumber(markScale);
        if (Scale == Number(0.0f)) Scale = 1.0f;
    }

    Number Result = A + (T / Scale) * (B - A);

    // 2. Store result at {0} using Output node index and target type
    StoreScalar(OutMark, Result, resLow.Type);
    return true;
}