bool Delay(const Bookmark &OpPoint)
{
    // 1. Navigation: {0} Out, {1} DelayMs, {2} Mode, {3} TimerState
    Bookmark OutMark    = OpPoint.Child();
    if (OutMark.Index == INVALID_HEADER) return true;

    Bookmark markDelay  = OutMark.Next();  // {1}
    Bookmark markMode   = markDelay.Next(); // {2}
    Bookmark markState  = markMode.Next();  // {3}

    if (markDelay.Index == INVALID_HEADER || markState.Index == INVALID_HEADER)
        return true;

    // 2. Extract Values
    Result resDelay = markDelay.GetThis();
    Result resState = markState.GetThis();
    
    if (resDelay.Type != Types::Integer || !resDelay.Value)
        return true;

    int32_t DelayMs = *(int32_t*)resDelay.Value;
    int32_t StartTime = (resState.Type == Types::Integer && resState.Value) ? *(int32_t*)resState.Value : 0;

    if ((uint32_t)StartTime > CurrentTime)
    {
        StartTime = 0;
        int32_t Reset = 0;
        markState.SetCurrent(&Reset, sizeof(int32_t), Types::Integer);
    }
    
    // Resolve Mode (default to Remaining if node missing or false)
    bool isElapsedMode = false;
    Result resMode = markMode.GetThis();
    if (resMode.Type == Types::Bool && resMode.Value)
        isElapsedMode = *(bool*)resMode.Value;

    // 3. Logic Execution
    if (StartTime == 0)
    {
        // First hit: Capture start time
        markState.SetCurrent(&CurrentTime, sizeof(int32_t), Types::Integer);
        int32_t InitialOut = (resMode.Type == Types::Bool && *(bool*)resMode.Value) ? 0 : DelayMs;
        OutMark.SetCurrent(&InitialOut, sizeof(int32_t), Types::Integer);
        return false;
    }

    int32_t Elapsed = CurrentTime - StartTime;

    if (Elapsed >= DelayMs)
    {
        // Time Finished: Reset state and allow execution
        int32_t Reset = 0;
        markState.SetCurrent(&Reset, sizeof(int32_t), Types::Integer);
        
        // Output final state (0 remaining or Max elapsed)
        int32_t FinalOut = isElapsedMode ? DelayMs : 0;
        OutMark.SetCurrent(&FinalOut, sizeof(int32_t), Types::Integer);
        
        return true;
    }

    // 4. Update Output node while waiting
    int32_t CalculatedOut = isElapsedMode ? Elapsed : (DelayMs - Elapsed);
    OutMark.SetCurrent(&CalculatedOut, sizeof(int32_t), Types::Integer);

    // Still waiting: block execution chain
    return false;
}