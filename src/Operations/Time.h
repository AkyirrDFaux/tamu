bool Delay(const Bookmark &OpPoint)
{
    // 1. Navigate to nodes
    // {0} Output, {1} Inputs Folder, {2} State Folder
    Bookmark OutMark    = OpPoint.Map->Child(OpPoint);
    Bookmark InFolder   = OpPoint.Map->Next(OutMark);
    Bookmark StateFolder = OpPoint.Map->Next(InFolder);

    if (InFolder.Index == INVALID_HEADER || StateFolder.Index == INVALID_HEADER)
        return true;

    // 2. Resolve specific bookmarks: {1, 0} for Delay, {2, 0} for Timer State
    Bookmark delayMark = OpPoint.Map->Child(InFolder);
    Bookmark stateMark = OpPoint.Map->Child(StateFolder);

    Result delayRes = OpPoint.Map->GetThis(delayMark);
    Result stateRes = OpPoint.Map->GetThis(stateMark);

    // If input isn't an integer or doesn't exist, skip the delay logic
    if (delayRes.Type != Types::Integer || !delayRes.Value)
        return true;

    // 3. Logic Execution
    int32_t Timer   = (stateRes.Type == Types::Integer && stateRes.Value) ? *(int32_t*)stateRes.Value : 0;
    int32_t DelayMs = *(int32_t*)delayRes.Value;

    if (Timer == 0)
    {
        // First hit: Capture start time and block execution
        OpPoint.Map->Set(&CurrentTime, sizeof(int32_t), Types::Integer, stateMark.Index);
        return false;
    }
    
    if (CurrentTime >= (Timer + DelayMs))
    {
        // Time elapsed: Reset timer for next cycle and allow execution
        int32_t Reset = 0;
        OpPoint.Map->Set(&Reset, sizeof(int32_t), Types::Integer, stateMark.Index);
        return true;
    }

    // Still waiting
    return false;
}

bool AddDelay(const Bookmark &OpPoint)
{
    // 1. Navigate to nodes
    // {0} Output, {1} Inputs Folder
    Bookmark OutMark  = OpPoint.Map->Child(OpPoint);
    Bookmark InFolder = OpPoint.Map->Next(OutMark);
    
    if (InFolder.Index == INVALID_HEADER || OutMark.Index == INVALID_HEADER)
        return true;

    // 2. Resolve input {1, 0}
    Bookmark inputMark = OpPoint.Map->Child(InFolder);
    Result inputRes    = OpPoint.Map->GetThis(inputMark);

    if (inputRes.Type != Types::Integer || !inputRes.Value)
        return true;

    // 3. Calculation
    int32_t FutureTime = *(int32_t*)inputRes.Value + CurrentTime;
    
    // Store result in the Output node {0}
    OpPoint.Map->Set(&FutureTime, sizeof(int32_t), Types::Integer, OutMark.Index);
    return true;
}