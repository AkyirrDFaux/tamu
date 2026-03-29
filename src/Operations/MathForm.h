bool Sine(ByteArray &Values, Reference Index)
{
    Reference InTime = Index.Append(1).Append(0);
    Reference InMult = Index.Append(1).Append(1);
    Reference InPhase = Index.Append(1).Append(2);
    Reference Output = Index.Append(0);

    if (Values.Type(InTime) != Types::Integer)
        return true;

    int32_t T = Values.Get<int32_t>(InTime).Value;
    Number M = Values.Get<Number>(InMult).Value;
    Number P = Values.Get<Number>(InPhase).Value;

    Values.Set(sin(T * M + P), Output);
    return true;
}