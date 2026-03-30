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

    // 1. Locate the first parameter (Index + {1, 0})
    // This starts the search at the input branch and dives to the first child
    SearchResult FirstParam = Values.Find(Index.Append(1).Append(0), true);
    if (!FirstParam.Value) return true;

    Types Target = Types::Undefined;

    // --- PASS 1: Identify Target Type ---
    // Walk siblings using the Next() primitive
    SearchResult it = FirstParam;
    while (it.Value)
    {
        // Resolve references to see the underlying data type
        SearchResult actual = Values.This(it.Location);
        
        if (Target == Types::Undefined) Target = actual.Type;
        else Target = MergeTypes(Target, actual.Type);

        if (Target == Types::Undefined) return true;
        
        it = Values.Next(it.Location);
    }

    if (Target == Types::Undefined) return true;

    // --- PASS 2: Accumulate ---
    // Start again at the first parameter
    it = FirstParam;
    
    // Initialize accumulator with the first value
    Arithmetic Result = Fetch(Values, it.Location, Target);
    
    // Move to second sibling to start the loop
    it = Values.Next(it.Location);

    while (it.Value)
    {
        Arithmetic NextVal = Fetch(Values, it.Location, Target);
        Result.Apply(NextVal, Op);

        if (Result.Type == Types::Undefined) return true;
        
        it = Values.Next(it.Location);
    }

    // --- PASS 3: Store ---
    StoreArithmetic(Values, OutPath, Result);
    return true;
}

bool ExecuteExtreme(ByteArray &Values, Reference Index, bool IsMax)
{
    Reference OutPath = Index.Append(0);

    // 1. Locate the first parameter (Index + {1, 0})
    SearchResult it = Values.Find(Index.Append(1).Append(0), true);
    if (!it.Value) return true;

    Number result = 0;
    bool foundFirst = false;

    // 2. Iterate siblings using Next()
    while (it.Value)
    {
        // Resolve references (local or global) to get actual POD data
        SearchResult actual = Values.This(it.Location);
        
        Number currentVal = 0;
        bool isScalar = true;

        // Scalar Extraction
        if (actual.Type == Types::Number) {
            currentVal = *(Number *)actual.Value;
        } else if (actual.Type == Types::Integer) {
            currentVal = (Number)(*(int32_t *)actual.Value);
        } else if (actual.Type == Types::Byte || actual.Type == Types::Bool) {
            currentVal = (Number)(*(uint8_t *)actual.Value);
        } else {
            isScalar = false;
        }

        if (isScalar) {
            if (!foundFirst) {
                result = currentVal;
                foundFirst = true;
            } else {
                if (IsMax) {
                    if (currentVal > result) result = currentVal;
                } else {
                    if (currentVal < result) result = currentVal;
                }
            }
        }

        // Move to the next sibling at the same depth
        it = Values.Next(it.Location);
    }

    // 3. Store result if at least one valid scalar was found
    if (foundFirst) {
        Values.Set(&result, sizeof(Number), Types::Number, OutPath);
    }

    return true;
}