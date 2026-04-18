struct OpContext {
    Bookmark Out;           // {0} Output node
    Result Args[8];         // Raw data/metadata for quick access
    Bookmark ArgMarks[8];   // Direct bookmarks to input nodes {1...8}
    Operations Op;          // The specific operation ID
};

Number GetAsNumber(const Result &Source)
{
    if (!Source.Value || Source.Type == Types::Undefined)
        return N(0.0);

    switch (Source.Type)
    {
    case Types::Number:
        return *(Number *)Source.Value;

    case Types::Integer:
        return (Number)(*(int32_t *)Source.Value);

    case Types::Byte:
    case Types::Bool:
        return (Number)(*(uint8_t *)Source.Value);

    default:
        // For complex types, we could extract Magnitude if we wanted to be fancy,
        // but for now, 0.0 is the safest fallback for math.
        return N(0.0);
    }
}

void StoreScalar(const Bookmark &Target, Number Result, Types T)
{
    if (Target.Index == INVALID_HEADER)
        return;

    if (T == Types::Integer)
    {
        int32_t val = Result.RoundToInt();
        Target.SetCurrent(&val, sizeof(int32_t), T);
    }
    else if (T == Types::Byte) // Separate Byte
    {
        uint8_t val = (uint8_t)LimitByte(Result.RoundToInt());
        Target.SetCurrent(&val, 1, T);
    }
    else if (T == Types::Bool) // Handle Bool conversion explicitly
    {
        // If the target is a bool, we store 0 or 1
        uint8_t val = (Result.Value != 0) ? 1 : 0;
        Target.SetCurrent(&val, 1, T);
    }
    else if (T == Types::Number)
    {
        Target.SetCurrent(&Result, sizeof(Number), T);
    }
}

bool ExecuteSet(OpContext &ctx)
{
    // In our standardized dispatcher:
    // ctx.Out is {0}
    // ctx.Args[0] is the result of {1} (the first sibling of the output)

    if (ctx.Args[0].Value && ctx.Args[0].Length > 0)
    {
        // Direct copy from the first input to the output
        ctx.Out.SetCurrent(ctx.Args[0].Value, ctx.Args[0].Length, ctx.Args[0].Type);
    }
    
    return true;
}

bool ExecuteDelete(OpContext &ctx)
{
    // ctx.ArgMarks[0...7] = Bookmarks to the targets we want to delete
    
    for (uint8_t i = 0; i < 8; i++)
    {
        if (ctx.ArgMarks[i].Index == INVALID_HEADER)
            continue;

        // 1. Get the Reference snapshot from the bookmark
        Result res = ctx.ArgMarks[i].Get();
        if (res.Type != Types::Reference || !res.Value)
            continue;

        // 2. Resolve the target via the Bookmark's internal resolution logic
        // We use This() to get the bookmark pointing to the ACTUAL data
        Bookmark Target = ctx.ArgMarks[i].This();

        // 3. Execution: Tell the Map to remove the existing index
        if (Target.Map != nullptr && Target.Index != INVALID_HEADER)
        {
            // This clears the data and potentially shifts the map memory
            Target.Map->Delete(Target.Index);
        }
    }

    // Since we are deleting things, the tree layout might have changed.
    // Returning true allows the dispatcher to finish, but be careful 
    // about using these references again in the same frame!
    return true;
}