bool ExecuteDelay(OpContext &ctx)
{
    // ctx.Out       = {0} Output (Calculated Time Remaining/Elapsed)
    // ctx.Args[0]   = {1} DelayMs (Integer)
    // ctx.Args[1]   = {2} Mode (Bool: False = Countdown, True = Elapsed)
    // ctx.Args[2]   = {3} TimerState (Persistent Storage in the tree)
    // ctx.ArgMarks[2] = {3} Bookmark for persistent storage

    if (ctx.Args[0].Type != Types::Integer || !ctx.Args[0].Value)
        return true;

    int32_t DelayMs = *(int32_t*)ctx.Args[0].Value;
    
    // Check if we have a valid StartTime in the state node
    int32_t StartTime = (ctx.Args[2].Type == Types::Integer && ctx.Args[2].Value) 
                        ? *(int32_t*)ctx.Args[2].Value : 0;

    // Handle Millis Overflow/Wrap-around
    if ((uint32_t)StartTime > CurrentTime) StartTime = 0;

    bool isElapsedMode = (ctx.Args[1].Type == Types::Bool && ctx.Args[1].Value) 
                         ? *(bool*)ctx.Args[1].Value : false;

    // 1. Initial Trigger
    if (StartTime == 0)
    {
        // Save current timestamp directly into the tree node for Arg {3}
        ctx.ArgMarks[2].SetCurrent(&CurrentTime, sizeof(int32_t), Types::Integer);

        int32_t InitialOut = isElapsedMode ? 0 : DelayMs;
        ctx.Out.SetCurrent(&InitialOut, sizeof(int32_t), Types::Integer);
        return false; // Block execution
    }

    int32_t Elapsed = CurrentTime - StartTime;

    // 2. Timer Finished
    if (Elapsed >= DelayMs)
    {
        // Reset the State node {3} to 0 via direct bookmark access
        int32_t Reset = 0;
        ctx.ArgMarks[2].SetCurrent(&Reset, sizeof(int32_t), Types::Integer);
        
        int32_t FinalOut = isElapsedMode ? DelayMs : 0;
        ctx.Out.SetCurrent(&FinalOut, sizeof(int32_t), Types::Integer);
        
        return true; // Timer done, allow execution chain to proceed
    }

    // 3. Timer Running: Update Output node with progress
    int32_t CalculatedOut = isElapsedMode ? Elapsed : (DelayMs - Elapsed);
    ctx.Out.SetCurrent(&CalculatedOut, sizeof(int32_t), Types::Integer);

    return false; // Still waiting, block execution
}