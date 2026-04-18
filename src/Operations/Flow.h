bool ExecuteIfSwitch(OpContext &ctx)
{
    // ctx.Args[0] = {1} Index Selector (Integer)
    // ctx.Args[1...7] = {2...8} Potential Data Sources

    if (ctx.Args[0].Type != Types::Integer || !ctx.Args[0].Value)
        return true;

    int32_t selector = *(int32_t *)ctx.Args[0].Value;

    // Boundary check: we only pre-fetched 7 data options (index 1 to 7)
    if (selector < 0 || selector > 6)
        return true;

    // Offset by 1 because the selector is at Args[0]
    uint8_t sourceIdx = (uint8_t)selector + 1;
    Result selected = ctx.Args[sourceIdx];

    if (selected.Value && selected.Length > 0)
    {
        // Route the chosen input data to the output node
        ctx.Out.SetCurrent(selected.Value, selected.Length, selected.Type);
    }

    return true;
}

bool ExecuteSetRun(OpContext &ctx)
{
    // ctx.Args[0] = {1} Trigger/Enable (Bool)
    // ctx.ArgMarks[1...7] = {2...8} Bookmarks to target Operations

    if (ctx.Args[0].Type != Types::Bool || !ctx.Args[0].Value)
        return true;

    bool active = *(bool *)ctx.Args[0].Value;

    for (uint8_t i = 1; i < 8; i++)
    {
        if (ctx.ArgMarks[i].Index == INVALID_HEADER)
            continue;

        // 1. Get the Reference ID from the Bookmark
        // (Assuming Bookmark has access to the Reference it represents)
        Reference ID = *(Reference *)ctx.ArgMarks[i].Get().Value;

        // 2. Locate in the Object Registry
        int32_t regIdx = Objects.Search(ID);
        if (regIdx == -1)
            continue;

        BaseClass *Obj = Objects.Object[regIdx].Object;

        if (!Obj)
            continue;

        // 3. Apply the Timing Logic
        if (ctx.Op == Operations::SetRunOnce)
        {
            active ? Obj->Flags += Flags::RunOnce : Obj->Flags -= Flags::RunOnce;
        }
        else if (ctx.Op == Operations::SetRunLoop)
        {
            active ? Obj->Flags += Flags::RunLoop : Obj->Flags -= Flags::RunLoop;
        }

        // 4. Critical: Sync the Runner Registry
        // This ensures the hardware scheduler sees the change immediately
        Objects.Object[regIdx].Info = {Obj->RunPeriod, Obj->RunPhase};
    }

    return true;
}