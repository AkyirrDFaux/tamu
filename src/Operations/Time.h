bool Delay(ByteArray &Values, Reference Index)
{
    Reference InputPath = Index.Append(1).Append(0);
    Reference StatePath = Index.Append(2).Append(0); // Persistent state

    SearchResult DelayRes = Values.Find(InputPath);
    SearchResult StateRes = Values.Find(StatePath, true);

    // If input isn't an integer or doesn't exist, skip the delay
    if (DelayRes.Type != Types::Integer || !DelayRes.Value)
        return true;

    // Default timer to 0 if not yet initialized or invalid
    int32_t Timer = (StateRes.Type == Types::Integer && StateRes.Value) ? *(int32_t*)StateRes.Value : 0;
    int32_t DelayMs = *(int32_t*)DelayRes.Value;

    if (Timer == 0)
    {
        // First hit: Capture start time and block execution
        Values.Set(&CurrentTime, sizeof(int32_t), Types::Integer, StatePath);
        return false;
    }
    
    if (CurrentTime >= (Timer + DelayMs))
    {
        // Time elapsed: Reset timer for next cycle and allow execution
        int32_t Reset = 0;
        Values.Set(&Reset, sizeof(int32_t), Types::Integer, StatePath);
        return true;
    }

    // Still waiting
    return false;
}

bool AddDelay(ByteArray &Values, Reference Index)
{
    Reference InputPath = Index.Append(1).Append(0);
    Reference OutPath   = Index.Append(0);

    SearchResult InputRes = Values.Find(InputPath);

    if (InputRes.Type != Types::Integer || !InputRes.Value)
        return true;

    // Calculate future timestamp: Input offset + Current system time
    int32_t FutureTime = *(int32_t*)InputRes.Value + CurrentTime;
    
    Values.Set(&FutureTime, sizeof(int32_t), Types::Integer, OutPath);
    return true;
}