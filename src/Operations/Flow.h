bool ExecuteIfSwitch(OpContext &ctx)
{
    // 1. The Selector index is now stored in this block's Out node
    Result outRes = ctx.Out.Get();
    if (outRes.Type != Types::Integer || !outRes.Value)
        return true;

    int32_t selector = *(int32_t *)outRes.Value;

    // Boundary check for Args[0...7] (which are the bookmarks)
    if (selector < 0 || selector > 7)
        return true;

    // 2. The arguments are strictly bookmarks to operations
    Bookmark targetMark = ctx.ArgMarks[selector];

    if (targetMark.Index != INVALID_HEADER)
    {
        // 3. Trigger the selected operation
        // This executes the logic branch. We do not set ctx.Out to the result,
        // as the Out node is reserved for the selector value itself.
        RunOperation(targetMark);
    }

    return true;
}

// TODO move bool to output (both)

bool ExecuteSetRun(OpContext &ctx)
{
    // 1. The 'active' bool is now moved to the Out node
    // This allows the SetRun state itself to be a data source for others
    Result outRes = ctx.Out.Get();
    if (outRes.Type != Types::Bool || !outRes.Value)
        return true;

    bool active = *(bool *)outRes.Value;

    // ctx.ArgMarks[1...7] = Operations to enable/disable
    for (uint8_t i = 0; i < 8; i++)
    {
        if (ctx.ArgMarks[i].Index == INVALID_HEADER)
            continue;

        // Get the Reference from the Bookmark
        Result refRes = ctx.ArgMarks[i].Get();
        if (refRes.Type != Types::Reference || !refRes.Value)
            continue;

        Reference ID = *(Reference *)refRes.Value;

        // Locate in Registry
        int32_t regIdx = Objects.Search(ID);
        if (regIdx == -1)
            continue;

        BaseClass *Obj = Objects.Object[regIdx].Object;
        if (!Obj)
            continue;

        // 3. Apply Flags
        if (ctx.Op == Operations::SetRunOnce)
        {
            if (active)
                Obj->Flags += Flags::RunOnce;
            else
                Obj->Flags -= Flags::RunOnce;
        }
        else if (ctx.Op == Operations::SetRunLoop)
        {
            if (active)
                Obj->Flags += Flags::RunLoop;
            else
                Obj->Flags -= Flags::RunLoop;
        }

        // 4. Sync Hardware Scheduler
        Objects.Object[regIdx].Info = {Obj->RunPeriod, Obj->RunPhase};
    }

    return true;
}