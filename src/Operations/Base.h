bool Equal(ByteArray &Values, Reference Index)
{
    /*Reference Input0 = Index.Append(1).Append(0);
    Reference Output = Index.Append(0);*/

    return true;
}

bool Combine(ByteArray &Values, Reference Index)
{
    Reference OutPath = Index.Append(0);
    Reference InPath = Index.Append(1);

    // Fetch up to 4 potential input types to check patterns
    Types T0 = Values.Type(InPath.Append(0));
    Types T1 = Values.Type(InPath.Append(1));
    Types T2 = Values.Type(InPath.Append(2));
    Types T3 = Values.Type(InPath.Append(3));

    // --- 1. Coord2D Patterns ---
    // Pattern: 2 Vector2D -> Coord2D (Pos, Direction/Scale)
    if (T0 == Types::Vector2D && T1 == Types::Vector2D) {
        Vector2D v0 = Values.Get<Vector2D>(InPath.Append(0)).Value;
        Vector2D v1 = Values.Get<Vector2D>(InPath.Append(1)).Value;
        Values.Set(Coord2D(v0, v1), OutPath);
        return true;
    }
    // Pattern: Vector2D + Number -> Coord2D (Pos, Rotation)
    if (T0 == Types::Vector2D && T1 == Types::Number) {
        Vector2D v0 = Values.Get<Vector2D>(InPath.Append(0)).Value;
        Number n1 = Values.Get<Number>(InPath.Append(1)).Value;
        Values.Set(Coord2D(v0, n1), OutPath);
        return true;
    }

    // --- 2. Vector Patterns ---
    // Pattern: 3 Numbers -> Vector3D
    if (T0 == Types::Number && T1 == Types::Number && T2 == Types::Number) {
        Number n0 = Values.Get<Number>(InPath.Append(0)).Value;
        Number n1 = Values.Get<Number>(InPath.Append(1)).Value;
        Number n2 = Values.Get<Number>(InPath.Append(2)).Value;
        Values.Set(Vector3D(n0, n1, n2), OutPath);
        return true;
    }
    // Pattern: 2 Numbers -> Vector2D
    if (T0 == Types::Number && T1 == Types::Number) {
        Number n0 = Values.Get<Number>(InPath.Append(0)).Value;
        Number n1 = Values.Get<Number>(InPath.Append(1)).Value;
        Values.Set(Vector2D(n0, n1), OutPath);
        return true;
    }

    // --- 3. Color Patterns ---
    // Pattern: 3 or 4 Bytes -> Color
    if (T0 == Types::Byte && T1 == Types::Byte && T2 == Types::Byte) {
        uint8_t b0 = Values.Get<uint8_t>(InPath.Append(0)).Value;
        uint8_t b1 = Values.Get<uint8_t>(InPath.Append(1)).Value;
        uint8_t b2 = Values.Get<uint8_t>(InPath.Append(2)).Value;
        uint8_t b3 = (T3 == Types::Byte) ? Values.Get<uint8_t>(InPath.Append(3)).Value : 255;
        Values.Set(ColourClass(b0, b1, b2, b3), OutPath);
        return true;
    }

    return true;
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
        Done = Operation::Sine(Values, Index);
        break;
    default:
        ReportError(Status::InvalidValue);
        return true; // Or true, depending on if you want to stop execution
    }

    // 2. Broadcast/Copy Output
    Reference OutputPath = Index.Append(0);
    FindResult Payload = Values.Find(OutputPath);

    if (Payload.Header < 0)
        return true;
    const Header &DataHead = Values.HeaderArray[Payload.Header];

    // Loop through all Output References (Linked Outputs)
    for (uint8_t i = 0;; i++)
    {
        Getter<Reference> Ref = Values.Get<Reference>(OutputPath.Append(i));

        if (!Ref.Success)
            break;

        BaseClass *Target = nullptr;

        // Resolve context: Local or Global?
        if (Ref.Value.IsGlobal())
            Target = Objects.At(Ref.Value); // Ensure IsGlobal checks are robust
        else
            Target = Object;

        if (Target)
        {
            // Push the raw bytes to the target
            Target->Values.Set(
                Payload.Value,
                DataHead.Length,
                DataHead.Type,
                Ref.Value);

            // Trigger internal logic update on target
            Target->Setup(Ref.Value);
        }
    }

    return true;
}