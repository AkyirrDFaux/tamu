bool Equal(const Bookmark &OpPoint)
{
    // 1. Navigation: Output is {0}, Inputs Folder is {1}
    Bookmark OutMark = OpPoint.Child();
    Bookmark InFolder = OutMark.Next();

    if (InFolder.Index == INVALID_HEADER || OutMark.Index == INVALID_HEADER)
        return false;

    // 2. Source is the first child of the Inputs Folder {1, 0}
    Bookmark SrcMark = InFolder.Child();
    Result Source = SrcMark.GetThis();

    if (Source.Value && Source.Length > 0)
    {
        // 3. Direct write to the Output node using its Index
        OutMark.Set(Source.Value, Source.Length, Source.Type, true);
        return true;
    }
    return false;
}

bool Combine(const Bookmark &OpPoint)
{
    // 1. Navigation
    Bookmark OutMark = OpPoint.Child();
    Bookmark InFolder = OutMark.Next();
    
    if (InFolder.Index == INVALID_HEADER || OutMark.Index == INVALID_HEADER) 
        return false;

    // 2. Collect up to 4 input siblings
    Bookmark Marks[4];
    Result S[4];

    Marks[0] = InFolder.Child();
    for (uint8_t i = 1; i < 4; i++)
        Marks[i] = Marks[i-1].Next();

    for (uint8_t i = 0; i < 4; i++)
    {
        if (Marks[i].Index != INVALID_HEADER)
            S[i] = Marks[i].GetThis();
    }

    // 3. Pattern Matching and Direct Set
    // We use OutMark.Index to store the result directly in the tree

    // Pattern: 3 or 4 Bytes/Bools -> Colour
    if (S[0].Type == Types::Byte && S[1].Type == Types::Byte && S[2].Type == Types::Byte)
    {
        uint8_t b0 = (uint8_t)GetAsNumber(Marks[0]);
        uint8_t b1 = (uint8_t)GetAsNumber(Marks[1]);
        uint8_t b2 = (uint8_t)GetAsNumber(Marks[2]);
        uint8_t b3 = (S[3].Type == Types::Byte) ? (uint8_t)GetAsNumber(Marks[3]) : 255;

        ColourClass col(b0, b1, b2, b3);
        OutMark.Set(&col, sizeof(ColourClass), Types::Colour, true);
        return true;
    }

    // Pattern: 2 Vector2D -> Coord2D
    if (S[0].Type == Types::Vector2D && S[1].Type == Types::Vector2D)
    {
        Coord2D c(*(Vector2D *)S[0].Value, *(Vector2D *)S[1].Value);
        OutMark.Set(&c, sizeof(Coord2D), Types::Coord2D, true);
        return true;
    }

    // Pattern: Vector2D + Scalar -> Coord2D
    if (S[0].Type == Types::Vector2D && IsScalar(S[1].Type))
    {
        Coord2D c(*(Vector2D *)S[0].Value, GetAsNumber(Marks[1]));
        OutMark.Set(&c, sizeof(Coord2D), Types::Coord2D, true);
        return true;
    }

    // Pattern: 3 Scalars -> Vector3D
    if (IsScalar(S[0].Type) && IsScalar(S[1].Type) && IsScalar(S[2].Type))
    {
        Vector3D v(GetAsNumber(Marks[0]), GetAsNumber(Marks[1]), GetAsNumber(Marks[2]));
        OutMark.Set(&v, sizeof(Vector3D), Types::Vector3D, true);
        return true;
    }

    // Pattern: 2 Scalars -> Vector2D
    if (IsScalar(S[0].Type) && IsScalar(S[1].Type))
    {
        Vector2D v(GetAsNumber(Marks[0]), GetAsNumber(Marks[1]));
        OutMark.Set(&v, sizeof(Vector2D), Types::Vector2D, true);
        return true;
    }

    return false;
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
    case Operations::AddDelay:
        Done = AddDelay(Index);
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

        Result Link = linkMark.Get();

        if (Link.Length == 0 || Link.Type != Types::Reference)
            break;

        Reference TargetPath = *(Reference *)Link.Value;
        if (!TargetPath.IsValid())
            continue;

        Index.Map->Set(Payload.Value, Payload.Length, Payload.Type, TargetPath);

        linkMark = linkMark.Next();
    }

    return Done;
}