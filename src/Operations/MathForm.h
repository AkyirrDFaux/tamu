bool ExecuteWaveforms(OpContext &ctx)
{
    // ctx.Args[0] = Time/Input, [1] = Frequency/Multiplier, [2] = Phase
    Number T = GetAsNumber(ctx.Args[0]);
    Number M = GetAsNumber(ctx.Args[1]);
    Number P = GetAsNumber(ctx.Args[2]);
    Number PhaseTime = (T * M) + P;

    Number result = N(0.0);

    switch (ctx.Op)
    {
    case Operations::Sine:
        // Assuming your 'Number' lib has sin or you use math.h
        result = Number(sin(PhaseTime));
        break;

    case Operations::Square:
        // Simple sign-based square wave
        result = (Number(sin(PhaseTime)) >= N(0.0)) ? N(1.0) : N(-1.0);
        break;

    case Operations::Sawtooth:
        // Simple (T % 1.0) normalized sawtooth
        result = PhaseTime - PhaseTime.ToInt();
        break;

    default:
        break;
    }

    ctx.Out.SetCurrent(&result, sizeof(Number), Types::Number);
    return true;
}

bool ExecuteRandom(OpContext &ctx)
{
    // Default to 0 and 1 if arguments are missing
    Number minRange = (ctx.Args[0].Value) ? *(Number *)ctx.Args[0].Value : Number(0);
    Number maxRange = (ctx.Args[1].Value) ? *(Number *)ctx.Args[1].Value : Number(1);

    if (minRange >= maxRange)
    {
        ctx.Out.SetCurrent(&minRange, sizeof(Number), Types::Number);
        return true;
    }

    // Calculation: Result = Min + ((Max - Min) * (RandomValue / 1.0))
    // To avoid float, we use the raw random bits as the fraction.

    uint32_t range = (uint32_t)(maxRange.Value - minRange.Value);

    // Perform a fixed-point multiplication of the range and a random fraction
    // (RawRand() & 0xFFFF) acts as a value between 0 and 65535
    uint32_t fraction = RawRand() & 0xFFFF;
    uint32_t offset = (uint32_t)(((uint64_t)range * fraction) >> 16);

    Number result = Number::FromRaw(minRange.Value + (int32_t)offset);

    ctx.Out.SetCurrent(&result, sizeof(Number), Types::Number);
    return true;
}

bool ExecuteClamp(OpContext &ctx)
{
    // ctx.Args[0] = Value, [1] = Min, [2] = Max
    if (!IsScalar(ctx.Args[0].Type))
        return true;

    Number V = GetAsNumber(ctx.Args[0]);
    Number Low = GetAsNumber(ctx.Args[1]);
    Number High = GetAsNumber(ctx.Args[2]);

    if (V < Low)
        V = Low;
    else if (V > High)
        V = High;

    StoreScalar(ctx.Out, V, ctx.Args[0].Type);
    return true;
}

bool ExecuteDeadzone(OpContext &ctx)
{
    // ctx.Args[0] = Value, [1] = Threshold
    if (!IsScalar(ctx.Args[0].Type))
        return true;

    Number V = GetAsNumber(ctx.Args[0]);
    Number Threshold = abs(GetAsNumber(ctx.Args[1])); // Ensure positive

    Number Result = V;

    if (abs(V) < Threshold)
    {
        Result = N(0.0);
    }
    else if (V > N(0.0))
    {
        Result = (V - Threshold);
    }
    else
    {
        Result = (V + Threshold);
    }

    StoreScalar(ctx.Out, Result, ctx.Args[0].Type);
    return true;
}

bool ExecuteLerp(OpContext &ctx)
{
    // ctx.Args[0]=Low, [1]=High, [2]=Alpha, [3]=Scale (Optional)
    if (!IsScalar(ctx.Args[0].Type))
        return true;

    Number A = GetAsNumber(ctx.Args[0]);
    Number B = GetAsNumber(ctx.Args[1]);
    Number T = GetAsNumber(ctx.Args[2]);

    // Handle optional scale parameter
    Number Scale = (ctx.Args[3].Value) ? GetAsNumber(ctx.Args[3]) : N(1.0);
    if (Scale == N(0.0))
        Scale = N(1.0); // Div zero safety

    Number ResultValue = A + (T / Scale) * (B - A);

    StoreScalar(ctx.Out, ResultValue, ctx.Args[0].Type);
    return true;
}