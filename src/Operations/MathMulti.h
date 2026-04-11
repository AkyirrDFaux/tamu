bool ExecuteVector3DMulti(const Bookmark &itStart, const Bookmark &Out, Operations Op)
{
    Result first = itStart.GetThis();
    if (!first.Value)
        return true;

    Vector3D acc = *(Vector3D *)first.Value;
    Bookmark it = itStart.Next();

    while (it.Index != INVALID_HEADER)
    {
        Result actual = it.GetThis();
        // If we hit something that isn't a Vector3D, we stop (or return true)
        if (actual.Type != Types::Vector3D || !actual.Value)
            return true;

        Vector3D nextVal = *(Vector3D *)actual.Value;
        switch (Op)
        {
        case Operations::Add:
            acc = acc + nextVal;
            break;
        case Operations::Subtract:
            acc = acc - nextVal;
            break;
        case Operations::Multiply:
            acc = acc * nextVal;
            break;
        case Operations::Divide:
            acc = acc / nextVal;
            break;
        default:
            return true;
        }
        it = it.Next();
    }

    Out.SetCurrent(&acc, sizeof(Vector3D), Types::Vector3D);
    return true;
}

bool ExecuteVector2DMulti(const Bookmark &itStart, const Bookmark &Out, Operations Op)
{
    Result first = itStart.GetThis();
    if (!first.Value)
        return true;

    Vector2D acc = *(Vector2D *)first.Value;
    Bookmark it = itStart.Next();

    while (it.Index != INVALID_HEADER)
    {
        Result actual = it.GetThis();
        if (actual.Type != Types::Vector2D || !actual.Value)
            return true;

        Vector2D nextVal = *(Vector2D *)actual.Value;
        switch (Op)
        {
        case Operations::Add:
            acc = acc + nextVal;
            break;
        case Operations::Subtract:
            acc = acc - nextVal;
            break;
        case Operations::Multiply:
            acc = acc * nextVal;
            break;
        case Operations::Divide:
            acc = acc / nextVal;
            break;
        default:
            return true;
        }
        it = it.Next();
    }

    Out.SetCurrent(&acc, sizeof(Vector2D), Types::Vector2D);
    return true;
}

bool ExecuteMathMulti(const Bookmark &OpPoint, Operations Op)
{
    // 1. Navigation: {0} is Output, {1} is the start of Sibling chain
    Bookmark OutMark = OpPoint.Child();
    if (OutMark.Index == INVALID_HEADER)
        return true;

    Bookmark it = OutMark.Next(); // This is the first input {1}
    if (it.Index == INVALID_HEADER)
        return true;

    Result firstActual = it.GetThis();
    if (!firstActual.Value)
        return true;

    Types targetType = firstActual.Type;

    // 2. Delegate Vector Lanes
    if (targetType == Types::Vector3D)
        return ExecuteVector3DMulti(it, OutMark, Op);

    if (targetType == Types::Vector2D)
        return ExecuteVector2DMulti(it, OutMark, Op);

    // 3. Scalar Lane
    if (!IsScalar(targetType))
        return true;

    Number acc = GetAsNumber(it);
    it = it.Next(); // Move to {2}

    while (it.Index != INVALID_HEADER)
    {
        Result actual = it.GetThis();
        if (!IsScalar(actual.Type))
            return true;

        Number nextVal = GetAsNumber(it);

        switch (Op)
        {
        case Operations::Add:
            acc += nextVal;
            break;
        case Operations::Subtract:
            acc -= nextVal;
            break;
        case Operations::Multiply:
            acc = acc * nextVal;
            break;
        case Operations::Divide:
            acc = acc / nextVal;
            break;
        case Operations::Max:
            if (nextVal > acc)
                acc = nextVal;
            break;
        case Operations::Min:
            if (nextVal < acc)
                acc = nextVal;
            break;
        default:
            return true;
        }
        it = it.Next();
    }

    // 4. Final storage using context-aware StoreScalar
    StoreScalar(OutMark, acc, targetType);
    return true;
}