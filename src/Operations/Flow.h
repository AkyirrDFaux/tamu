bool SetFlags(ByteArray &Values, Reference Index)
{
    /*if (Values.Type(1) != Types::Flags)
        return true;

    FlagClass Value = ValueGet<FlagClass>(1);

    for (uint32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        Modules.At(Index)->Flags += Value.Values;
    */
    return true;
}

bool ResetFlags(ByteArray &Values, Reference Index)
{
    /*if (Values.Type(1) != Types::Flags)
        return true;

    FlagClass Value = ValueGet<FlagClass>(1);

    for (uint32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        Modules.At(Index)->Flags -= Value.Values;
    */
    return true;
}