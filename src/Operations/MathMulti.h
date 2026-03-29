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

    // 1. Locate the Input Branch 
    SearchResult BranchRes = Values.Find(Index.Append(1));
    if (!BranchRes.Location.Map) return true;
    
    Bookmark InBranch = BranchRes.Location;
    Types Target = Types::Undefined;
    uint8_t count = 0;

    // --- PASS 1: Identify Target Type ---
    while (true)
    {
        SearchResult res = Values.Find(InBranch, Reference({count}));
        if (res.Type == Types::Undefined || !res.Value) break;

        if (Target == Types::Undefined) Target = res.Type;
        else Target = MergeTypes(Target, res.Type);

        if (Target == Types::Undefined) return true;
        count++;
    }

    if (count == 0 || Target == Types::Undefined) return true;

    // --- PASS 2: Accumulate ---
    // Pass the Bookmark and the relative index {i}
    Arithmetic Result = Fetch(Values, InBranch, Reference({0}), Target);

    for (uint8_t i = 1; i < count; i++)
    {
        Arithmetic Next = Fetch(Values, InBranch, Reference({i}), Target);
        Result.Apply(Next, Op);

        if (Result.Type == Types::Undefined) return true;
    }

    // --- PASS 3: Store ---
    StoreArithmetic(Values, OutPath, Result);
    return true;
}

bool Min(ByteArray &Values, Reference Index)
{
    Reference OutPath = Index.Append(0);
    SearchResult BranchRes = Values.Find(Index.Append(1));

    if (!BranchRes.Location.Map) return true;
    Bookmark InBranch = BranchRes.Location;

    Number Result = 0;
    bool foundFirst = false;

    for (uint8_t i = 0; i < 255; i++)
    {
        // New Find dives automatically: i=0 is the first child
        SearchResult res = Values.Find(InBranch, Reference({i}));
        if (res.Type == Types::Undefined || !res.Value) break;

        Number currentVal = 0;
        bool isScalar = true;

        if (res.Type == Types::Number)           currentVal = *(Number *)res.Value;
        else if (res.Type == Types::Integer)     currentVal = (Number)(*(int32_t *)res.Value);
        else if (res.Type == Types::Byte || res.Type == Types::Bool) 
                                                 currentVal = (Number)(*(uint8_t *)res.Value);
        else isScalar = false;

        if (isScalar) {
            if (!foundFirst) {
                Result = currentVal;
                foundFirst = true;
            } else if (currentVal < Result) {
                Result = currentVal;
            }
        }
    }

    if (foundFirst) Values.Set(&Result, sizeof(Number), Types::Number, OutPath);
    return true;
}

bool Max(ByteArray &Values, Reference Index)
{
    Reference OutPath = Index.Append(0);
    SearchResult BranchRes = Values.Find(Index.Append(1));

    if (!BranchRes.Location.Map) return true;
    Bookmark InBranch = BranchRes.Location;

    Number Result = 0;
    bool foundFirst = false;

    for (uint8_t i = 0; i < 255; i++)
    {
        SearchResult res = Values.Find(InBranch, Reference({i}));
        if (res.Type == Types::Undefined || !res.Value) break;

        Number currentVal = 0;
        bool isScalar = true;

        if (res.Type == Types::Number)           currentVal = *(Number *)res.Value;
        else if (res.Type == Types::Integer)     currentVal = (Number)(*(int32_t *)res.Value);
        else if (res.Type == Types::Byte || res.Type == Types::Bool) 
                                                 currentVal = (Number)(*(uint8_t *)res.Value);
        else isScalar = false;

        if (isScalar) {
            if (!foundFirst) {
                Result = currentVal;
                foundFirst = true;
            } else if (currentVal > Result) {
                Result = currentVal;
            }
        }
    }

    if (foundFirst) Values.Set(&Result, sizeof(Number), Types::Number, OutPath);
    return true;
}