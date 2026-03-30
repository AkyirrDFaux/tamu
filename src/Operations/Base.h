bool Equal(ByteArray &Values, Reference Index)
{
    // Source: {1, 0}, Destination: {0}
    SearchResult Source = Values.Find(Index.Append(1).Append(0));

    if (Source.Length > 0 && Source.Value)
    {
        Values.Set(Source.Value, Source.Length, Source.Type, Index.Append(0));
        return true;
    }
    return false;
}

bool Combine(ByteArray &Values, Reference Index)
{
    Reference OutPath = Index.Append(0);
    Reference InPath = Index.Append(1);

    // Fetch snapshots for up to 4 potential inputs
    SearchResult S0 = Values.Find(InPath.Append(0));
    SearchResult S1 = Values.Find(InPath.Append(1));
    SearchResult S2 = Values.Find(InPath.Append(2));
    SearchResult S3 = Values.Find(InPath.Append(3));

    // Pattern: 3 or 4 Bytes -> Color
    if (S0.Type == Types::Byte && S1.Type == Types::Byte && S2.Type == Types::Byte)
    {
        uint8_t b0 = *(uint8_t *)S0.Value;
        uint8_t b1 = *(uint8_t *)S1.Value;
        uint8_t b2 = *(uint8_t *)S2.Value;
        uint8_t b3 = (S3.Type == Types::Byte) ? *(uint8_t *)S3.Value : 255;

        ColourClass col(b0, b1, b2, b3);
        Values.Set(&col, sizeof(ColourClass), Types::Colour, OutPath);
        return true;
    }

    // Pattern: 3 Numbers -> Vector3D
    if (S0.Type == Types::Number && S1.Type == Types::Number && S2.Type == Types::Number)
    {
        Vector3D v(*(Number *)S0.Value, *(Number *)S1.Value, *(Number *)S2.Value);
        Values.Set(&v, sizeof(Vector3D), Types::Vector3D, OutPath);
        return true;
    }

    // Pattern: 2 Vector2D -> Coord2D
    if (S0.Type == Types::Vector2D && S1.Type == Types::Vector2D)
    {
        Coord2D c(*(Vector2D *)S0.Value, *(Vector2D *)S1.Value);
        Values.Set(&c, sizeof(Coord2D), Types::Coord2D, OutPath);
        return true;
    }

    // Pattern: Vector2D + Number -> Coord2D
    if (S0.Type == Types::Vector2D && S1.Type == Types::Number)
    {
        Coord2D c(*(Vector2D *)S0.Value, *(Number *)S1.Value);
        Values.Set(&c, sizeof(Coord2D), Types::Coord2D, OutPath);
        return true;
    }

    // Pattern: 2 Numbers -> Vector2D
    if (S0.Type == Types::Number && S1.Type == Types::Number)
    {
        Vector2D v(*(Number *)S0.Value, *(Number *)S1.Value);
        Values.Set(&v, sizeof(Vector2D), Types::Vector2D, OutPath);
        return true;
    }

    return false;
}

bool Run(Operations Operation, ByteArray &Values, BaseClass *Object, Reference Index)
{
    bool Done = true;

    // 1. Execute the Operation
    switch (Operation)
    {
    case Operations::Equal:
        Done = Operation::Equal(Values, Index);
        break;
    case Operations::Combine:
        Done = Operation::Combine(Values, Index);
        break;
    case Operations::IsEqual:
    case Operations::IsGreater:
    case Operations::IsSmaller:
        Done = Operation::ExecuteCompare(Values, Index, Operation);
        break;
    case Operations::Add:
    case Operations::Subtract:
    case Operations::Multiply:
    case Operations::Divide:
        Done = Operation::ExecuteMathMulti(Values, Index, Operation);
        break;
    case Operations::Min:
    case Operations::Max:
        Done = Operation::ExecuteExtreme(Values, Index, Operation == Operations::Max);
        break;
    case Operations::Delay:
        Done = Operation::Delay(Values, Index);
        break;
    case Operations::AddDelay:
        Done = Operation::AddDelay(Values, Index);
        break;
    case Operations::SetFlags:
        Done = Operation::SetFlags(Values, Index);
        break;
    case Operations::ResetFlags:
        Done = Operation::ResetFlags(Values, Index);
        break;
    case Operations::Sine:
        Done = Operation::ExecuteSine(Values, Index);
        break;
    default:
        ReportError(Status::InvalidValue);
        return true; // Or true, depending on if you want to stop execution
    }

    // 2. Broadcast Output
    Reference OutputPath = Index.Append(0);
    SearchResult Payload = Values.Find(OutputPath,true);

    // If there is no calculated result to send, stop here
    if (Payload.Length == 0 || !Payload.Value)
        return true;

    // 3. Loop through linked "Output References" stored at {Index, 0, i}
    for (uint8_t i = 0;; i++)
    {
        SearchResult Link = Values.Find(OutputPath.Append(i),true);

        // Stop if we hit a gap or the end of the output reference list
        if (Link.Length == 0 || Link.Type != Types::Reference)
            break;

        Reference TargetPath = *(Reference*)Link.Value;
        if (!TargetPath.IsValid()) continue;

        // Determine if target is this object or a global one
        BaseClass *Target = TargetPath.IsGlobal() ? Objects.At(TargetPath) : Object;

        if (Target)
        {
            // Push the payload raw bytes to the target's ByteArray
            Target->Values.Set(
                Payload.Value,
                Payload.Length,
                Payload.Type,
                TargetPath);

            // Trigger the target's logic update
            Target->Setup(TargetPath);
        }
    }

    return Done;
}