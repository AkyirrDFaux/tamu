bool ExecuteVector3DMulti(ByteArray &Values, SearchResult it, Reference Path, Operations Op)
{
    Vector3D acc = *(Vector3D *)it.Location.Map->This(it.Location).Value;

    it = it.Location.Map->Next(it.Location);
    while (it.Value)
    {
        SearchResult actual = it.Location.Map->This(it.Location);
        if (actual.Type != Types::Vector3D)
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
        it = it.Location.Map->Next(it.Location);
    }

    Values.Set(&acc, sizeof(Vector3D), Types::Vector3D, Path);
    return true;
}

bool ExecuteVector2DMulti(ByteArray &Values, SearchResult it, Reference Path, Operations Op)
{
    Vector2D acc = *(Vector2D *)it.Location.Map->This(it.Location).Value;

    it = it.Location.Map->Next(it.Location);
    while (it.Value)
    {
        SearchResult actual = it.Location.Map->This(it.Location);
        if (actual.Type != Types::Vector2D)
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
        it = it.Location.Map->Next(it.Location);
    }

    Values.Set(&acc, sizeof(Vector2D), Types::Vector2D, Path);
    return true;
}

bool ExecuteMathMulti(ByteArray &Values, Reference Index, Operations Op)
{
    Reference OutPath = Index.Append(0);

    SearchResult it = Values.Find(Index.Append(1).Append(0), true);
    if (!it.Value)
        return true;

    SearchResult firstActual = it.Location.Map->This(it.Location);
    if (!firstActual.Value)
        return true;

    Types targetType = firstActual.Type;

    // Delegate if it's a vector lane
    if (targetType == Types::Vector3D)
        return ExecuteVector3DMulti(Values, it, OutPath, Op);
    if (targetType == Types::Vector2D)
        return ExecuteVector2DMulti(Values, it, OutPath, Op);

    // Reject if the first element isn't a scalar either
    if (!IsScalar(targetType))
        return true;

    Number acc = GetAsNumber(it.Location);

    it = it.Location.Map->Next(it.Location);
    while (it.Value)
    {
        SearchResult actual = it.Location.Map->This(it.Location);

        // "Is Not Scalar" rejection
        if (!IsScalar(actual.Type))
            return true;

        Number nextVal = GetAsNumber(it.Location);

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

        it = it.Location.Map->Next(it.Location);
    }

    StoreScalar(Values, OutPath, acc, targetType);
    return true;
}