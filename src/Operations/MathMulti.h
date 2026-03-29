template <typename T>
T Calculate(T a, T b, Operations Op)
{
    switch (Op)
    {
    case Operations::Add:
        return a + b;
    case Operations::Subtract:
        return a - b;
    case Operations::Multiply:
        return a * b;
    case Operations::Divide:
        return a / b;
    default:
        return a;
    }
};

void Arithmetic::Apply(const Arithmetic &Next, Operations Op)
{
    // If either side is Undefined, the result is Undefined
    if (Type == Types::Undefined || Next.Type == Types::Undefined)
    {
        Type = Types::Undefined;
        return;
    }

    switch (Type)
    {
    case Types::Vector3D:
        Value.v3 = Calculate(Value.v3, Next.Value.v3, Op);
        break;
    case Types::Vector2D:
        Value.v2 = Calculate(Value.v2, Next.Value.v2, Op);
        break;
    case Types::Number:
        Value.n = Calculate(Value.n, Next.Value.n, Op);
        break;
    case Types::Integer:
        Value.i = Calculate(Value.i, Next.Value.i, Op);
        break;
    case Types::Byte:
        // Promote to int32 for calculation to prevent overflow wrap-around during intermediate steps
        Value.b = (uint8_t)Calculate((int32_t)Value.b, (int32_t)Next.Value.b, Op);
        break;
    default:
        break;
    }
}

bool ExecuteMathMulti(ByteArray &Values, Reference Index, Operations Op)
{
    Reference OutPath = Index.Append(0);
    Reference InPath = Index.Append(1);

    // --- PASS 1: Determine Dominant Type ---
    Types Target = Types::Byte; // Start with lowest possible
    uint8_t count = 0;

    while (true)
    {
        Types T = Values.Type(InPath.Append(count));
        if (T == Types::Undefined)
            break;

        if (count == 0)
            Target = T;
        else
            Target = MergeTypes(Target, T);

        // If at any point the types are incompatible, bail early
        if (Target == Types::Undefined)
            return true;

        count++;
    }

    if (count == 0)
        return true;

    // --- PASS 2: Accumulate ---
    Arithmetic Result = Fetch(Values, InPath.Append(0), Target);

    for (uint8_t i = 1; i < count; i++)
    {
        Arithmetic Next = Fetch(Values, InPath.Append(i), Target);
        Result.Apply(Next, Op);

        if (Result.Type == Types::Undefined)
            return true;
    }

    // --- FINAL PASS: Store Result ---
    if (Result.Type == Types::Undefined)
        return true;

    StoreArithmetic(Values, OutPath, Result);

    return true;
}

bool Min(ByteArray &Values, Reference Index)
{
    Reference Output = Index.Append(0);
    Reference InputBase = Index.Append(1);

    Getter<Number> First = Values.Get<Number>(InputBase.Append(0));
    if (!First.Success)
        return true;

    Number Result = First.Value;
    uint8_t i = 1;

    while (true)
    {
        Getter<Number> Next = Values.Get<Number>(InputBase.Append(i++));
        if (!Next.Success)
            break;
        if (Next.Value < Result)
            Result = Next.Value;
    }

    Values.Set(Result, Output);
    return true;
}

bool Max(ByteArray &Values, Reference Index)
{
    Reference Output = Index.Append(0);
    Reference InputBase = Index.Append(1);

    Getter<Number> First = Values.Get<Number>(InputBase.Append(0));
    if (!First.Success)
        return true;

    Number Result = First.Value;
    uint8_t i = 1;

    while (true)
    {
        Getter<Number> Next = Values.Get<Number>(InputBase.Append(i++));
        if (!Next.Success)
            break;
        if (Next.Value > Result)
            Result = Next.Value;
    }

    Values.Set(Result, Output);
    return true;
}