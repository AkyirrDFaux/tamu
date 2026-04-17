bool Equal(const Bookmark &OpPoint)
{
    // Output is {0}, Input is {1}
    Bookmark OutMark = OpPoint.Child();
    Bookmark InMark = OutMark.Next();

    if (OutMark.Index == INVALID_HEADER || InMark.Index == INVALID_HEADER)
        return true;

    Result Source = InMark.GetThis();

    if (Source.Value && Source.Length > 0)
    {
        // Direct write to {0} using the value from {1}
        OutMark.SetCurrent(Source.Value, Source.Length, Source.Type);
        return true;
    }
    return true;
}

bool Convert(const Bookmark &OpPoint)
{
    // {0} Output, {1} Input Value, {2} Target Type
    Bookmark Out0 = OpPoint.Child();
    Bookmark In1 = Out0.Next();
    Bookmark Type2 = In1.Next();

    if (Out0.Index == INVALID_HEADER || In1.Index == INVALID_HEADER || Type2.Index == INVALID_HEADER)
        return true;

    Result Source = In1.GetThis();
    Result TargetSpec = Type2.GetThis();

    if (!Source.Value || !IsScalar(Source.Type))
        return true;

    // Use the type defined in {2} to determine how to cast the value in {1}
    Number val = GetAsNumber(In1);
    Types targetType = *(Types *)TargetSpec.Value;

    switch (targetType)
    {
    case Types::Byte:
    {
        uint8_t b = val.RoundToInt();
        Out0.SetCurrent(&b, 1, Types::Byte);
        break;
    }
    case Types::Integer:
    {
        int32_t i = val.RoundToInt();
        Out0.SetCurrent(&i, 4, Types::Integer);
        break;
    }
    case Types::Number:
    {
        Out0.SetCurrent(&val, 4, Types::Number);
        break;
    }
    case Types::Bool:
    {
        bool b = val > 0.5f;
        Out0.SetCurrent(&b, 1, Types::Bool);
        break;
    }
    default:
        return true;
    }

    return true;
}

bool Combine(const Bookmark &OpPoint)
{
    // 1. Setup Output {0}
    Bookmark OutMark = OpPoint.Child();
    if (OutMark.Index == INVALID_HEADER)
        return true;

    // 2. Collect up to 4 input siblings {1, 2, 3, 4}
    Bookmark Marks[4];
    Result S[4];
    // --- FIX: Explicitly initialize the first input sibling ---
    Marks[0] = OutMark.Next(); 
    for (uint8_t i = 1; i < 4; i++)
    {
        if (Marks[i - 1].Index == INVALID_HEADER)
            Marks[i].Index = INVALID_HEADER;
        else
            Marks[i] = Marks[i - 1].Next();
    }
    // Capture results and ensure we have actual data
    for (uint8_t i = 0; i < 4; i++)
    {
        if (Marks[i].Index != INVALID_HEADER)
            S[i] = Marks[i].GetThis();
        else
            S[i] = Result(); // Defaults to nullptr and 0 length
    }

    // 3. Pattern Matching (Check .Value to avoid null dereference)
    // --- Pattern: 2 Vector2D -> Coord2D ---
    if (S[0].Type == Types::Vector2D && S[1].Type == Types::Vector2D)
    {
        if (S[0].Value && S[1].Value)
        {
            Coord2D c(*(Vector2D *)S[0].Value, *(Vector2D *)S[1].Value);
            OutMark.SetCurrent(&c, sizeof(Coord2D), Types::Coord2D);
        }
        return true;
    }

    // --- Pattern: Vector2D + Scalar -> Coord2D ---
    if (S[0].Type == Types::Vector2D && IsScalar(S[1].Type))
    {
        if (S[0].Value && S[1].Value)
        {
            Coord2D c(*(Vector2D *)S[0].Value, GetAsNumber(Marks[1]));
            OutMark.SetCurrent(&c, sizeof(Coord2D), Types::Coord2D);
        }
        return true;
    }

    // --- Pattern: 3 Scalars -> Vector3D ---
    if (IsScalar(S[0].Type) && IsScalar(S[1].Type) && IsScalar(S[2].Type))
    {
        if (S[0].Value && S[1].Value && S[2].Value)
        {
            Vector3D v(GetAsNumber(Marks[0]).RoundToInt(), GetAsNumber(Marks[1]).RoundToInt(), GetAsNumber(Marks[2]).RoundToInt());
            OutMark.SetCurrent(&v, sizeof(Vector3D), Types::Vector3D);
        }
        return true;
    }

    // --- Pattern: 3 or 4 Bytes -> Colour ---
    if (S[0].Type == Types::Byte && S[1].Type == Types::Byte && S[2].Type == Types::Byte)
    {
        if (S[0].Value && S[1].Value && S[2].Value)
        {
            uint8_t b3 = (S[3].Type == Types::Byte && S[3].Value) ? (uint8_t)GetAsNumber(Marks[3]).RoundToInt() : 255;
            ColourClass col((uint8_t)GetAsNumber(Marks[0]).RoundToInt(),
                            (uint8_t)GetAsNumber(Marks[1]).RoundToInt(),
                            (uint8_t)GetAsNumber(Marks[2]).RoundToInt(), b3);

            OutMark.SetCurrent(&col, sizeof(ColourClass), Types::Colour);
        }
        return true;
    }

    // --- Pattern: 2 Scalars -> Vector2D ---
    if (IsScalar(S[0].Type) && IsScalar(S[1].Type))
    {
        if (S[0].Value && S[1].Value)
        {
            Vector2D v(GetAsNumber(Marks[0]).RoundToInt(), GetAsNumber(Marks[1]).RoundToInt());
            OutMark.SetCurrent(&v, sizeof(Vector2D), Types::Vector2D);
        }
        return true;
    }

    return true;
}

bool Extract(const Bookmark &OpPoint)
{
    // {0} Output, {1} Composite Source, {2} Component Index
    Bookmark Out0 = OpPoint.Child();
    Bookmark In1 = Out0.Next();
    Bookmark Idx2 = In1.Next();

    if (Out0.Index == INVALID_HEADER || In1.Index == INVALID_HEADER || Idx2.Index == INVALID_HEADER)
        return true;

    Result S = In1.GetThis();
    if (!S.Value)
        return true;

    // Get the requested index (e.g., 0 for X/R, 1 for Y/G, etc.)
    uint8_t componentIdx = GetAsNumber(Idx2).RoundToInt();

    // --- Vector2D/3D Extraction ---
    if (S.Type == Types::Vector2D || S.Type == Types::Vector3D)
    {
        float *v = (float *)S.Value; // Cast to float array for easy indexing
        uint8_t max = (S.Type == Types::Vector2D) ? 2 : 3;

        if (componentIdx < max)
        {
            Out0.SetCurrent(&v[componentIdx], 4, Types::Number);
            return true;
        }
    }

    // --- Colour Extraction ---
    if (S.Type == Types::Colour)
    {
        uint8_t *c = (uint8_t *)S.Value; // R, G, B, A
        if (componentIdx < 4)
        {
            Out0.SetCurrent(&c[componentIdx], 1, Types::Byte);
            return true;
        }
    }

    // --- Coord2D Extraction ---
    if (S.Type == Types::Coord2D)
    {
        Coord2D *coord = (Coord2D *)S.Value;
        if (componentIdx == 0)
        {
            Out0.SetCurrent(&coord->Offset, sizeof(Vector2D), Types::Vector2D);
            return true;
        }
        else if (componentIdx == 1)
        {
            Out0.SetCurrent(&coord->Rotation, sizeof(Vector2D), Types::Vector2D);
            return true;
        }
    }

    return true;
}

bool Run(Operations OpCode, const Bookmark &Index)
{
    bool Done = true;

    // 1. Execute the Operation
    switch (OpCode)
    {
    case Operations::Equal:
        Done = Equal(Index);
        break;
    case Operations::Combine:
        Done = Combine(Index);
        break;
    case Operations::Extract:
        Done = Extract(Index);
        break;
    case Operations::Convert:
        Done = Convert(Index);
        break;
    case Operations::IsEqual:
    case Operations::IsGreater:
    case Operations::IsSmaller:
        Done = ExecuteCompare(Index, OpCode);
        break;
    case Operations::Add:
    case Operations::Subtract:
    case Operations::Multiply:
    case Operations::Divide:
    case Operations::Min:
    case Operations::Max:
        Done = ExecuteMathMulti(Index, OpCode);
        break;
    case Operations::Clamp:
        Done = ExecuteClamp(Index);
        break;
    case Operations::LinInterpolation:
        Done = ExecuteLerp(Index);
        break;
    case Operations::Delay:
        Done = Delay(Index);
        break;
    case Operations::SetFlags:
        Done = SetFlags(Index);
        break;
    case Operations::ResetFlags:
        Done = ResetFlags(Index);
        break;
    case Operations::Sine:
        Done = ExecuteSine(Index);
        break;
    default:
        ReportError(Status::InvalidValue);
        return true;
    }

    // 2. Broadcast Output
    Bookmark outMark = Index.Child();
    Result Payload = outMark.Get();

    if (Payload.Length == 0 || !Payload.Value)
        return true;

    Bookmark linkMark = outMark.Child();

    // 3. Dispatch to outputs
    for (uint8_t i = 0;; i++)
    {
        if (linkMark.Index == INVALID_HEADER)
            break;

        Result Link = linkMark.Get();

        if (Link.Length == 0 || Link.Type != Types::Reference)
            break;

        Bookmark Target = linkMark.This();

        if (Target.Map != nullptr && Target.Index != INVALID_HEADER)
        {
            Target.Map->SetExisting(Payload.Value, Payload.Length, Payload.Type, Target.Index);
        }

        linkMark = linkMark.Next();
    }

    return Done;
}