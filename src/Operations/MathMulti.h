bool ExecuteVector3DMulti(const Bookmark &itStart, uint32_t OutIndex, Operations Op)
{
    Result first = itStart.Map->GetThis(itStart);
    Vector3D acc = *(Vector3D *)first.Value;

    Bookmark it = itStart.Map->Next(itStart);
    while (it.Index != INVALID_HEADER)
    {
        Result actual = it.Map->GetThis(it);
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
        it = it.Map->Next(it);
    }

    itStart.Map->Set(&acc, sizeof(Vector3D), Types::Vector3D, OutIndex);
    return true;
}

bool ExecuteVector2DMulti(const Bookmark &itStart, uint32_t OutIndex, Operations Op)
{
    Result first = itStart.Map->GetThis(itStart);
    Vector2D acc = *(Vector2D *)first.Value;

    Bookmark it = itStart.Map->Next(itStart);
    while (it.Index != INVALID_HEADER)
    {
        Result actual = it.Map->GetThis(it);
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
        it = it.Map->Next(it);
    }

    itStart.Map->Set(&acc, sizeof(Vector2D), Types::Vector2D, OutIndex);
    return true;
}

bool ExecuteMathMulti(const Bookmark &OpPoint, Operations Op)
{
    // 1. Navigation: {0} is Output, {1} is Inputs folder
    Bookmark OutMark = OpPoint.Map->Child(OpPoint);
    Bookmark InFolder = OpPoint.Map->Next(OutMark);

    if (OutMark.Index == INVALID_HEADER || InFolder.Index == INVALID_HEADER)
        return true;

    // 2. Find the first input at {1, 0}
    Bookmark it = OpPoint.Map->Child(InFolder);
    if (it.Index == INVALID_HEADER)
        return true;

    Result firstActual = OpPoint.Map->GetThis(it);
    if (!firstActual.Value)
        return true;

    Types targetType = firstActual.Type;

    // 3. Delegate if it's a vector lane
    if (targetType == Types::Vector3D)
        return ExecuteVector3DMulti(it, OutMark.Index, Op);
    if (targetType == Types::Vector2D)
        return ExecuteVector2DMulti(it, OutMark.Index, Op);

    // 4. Scalar Lane
    if (!IsScalar(targetType))
        return true;

    Number acc = GetAsNumber(it);

    it = OpPoint.Map->Next(it);
    while (it.Index != INVALID_HEADER)
    {
        Result actual = OpPoint.Map->GetThis(it);

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

        it = OpPoint.Map->Next(it);
    }

    // 5. Final storage using updated StoreScalar
    StoreScalar(OutMark, acc, targetType);
    return true;
}