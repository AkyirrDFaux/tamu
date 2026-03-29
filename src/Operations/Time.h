bool Delay(ByteArray &Values, Reference Index)
{
    Reference InputDelay = Index.Append(1).Append(0);
    Reference StateTimer = Index.Append(2).Append(0); // Persistent state

    if (Values.Type(InputDelay) != Types::Integer)
        return true;

    int32_t Timer = Values.Get<int32_t>(StateTimer).Value;

    if (Timer == 0)
    {
        Values.Set(CurrentTime, StateTimer);
        return false;
    }
    else if (CurrentTime > Timer + Values.Get<int32_t>(InputDelay).Value)
    {
        Values.Set((int32_t)0, StateTimer); // Reset for next use
        return true;
    }

    return false;
}

bool AddDelay(ByteArray &Values, Reference Index)
{
    Reference Input0 = Index.Append(1).Append(0);
    Reference Output = Index.Append(0);

    if (Values.Type(Input0) != Types::Integer)
        return true;

    Values.Set(Values.Get<int32_t>(Input0).Value + CurrentTime, Output);
    return true;
}