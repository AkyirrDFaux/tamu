bool ExecuteCompare(const Bookmark &OpPoint, Operations Op)
{
    Bookmark OutMark = OpPoint.Child();
    if (OutMark.Index == INVALID_HEADER)
        return true;

    Bookmark Mark_A = OutMark.Next();
    Bookmark Mark_B = Mark_A.Next();

    if (Mark_A.Index == INVALID_HEADER || Mark_B.Index == INVALID_HEADER)
        return true;

    Result Res_A = Mark_A.GetThis();
    Result Res_B = Mark_B.GetThis();

    if (!IsScalar(Res_A.Type) || !IsScalar(Res_B.Type))
        return true;

    Number Left = GetAsNumber(Mark_A);
    Number Right = GetAsNumber(Mark_B);
    bool result = false;

    switch (Op)
    {
    case Operations::IsEqual:
        result = (Left == Right);
        break;
    case Operations::IsGreater:
        result = (Left > Right);
        break;
    case Operations::IsSmaller:
        result = (Left < Right);
        break;
    default:
        return true;
    }

    OutMark.SetCurrent(&result, sizeof(bool), Types::Bool);
    return true;
}