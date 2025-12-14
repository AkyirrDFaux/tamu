template <class C>
C *IDList::Get(int32_t Index)
{
    if (!IsValid(Index))
        return nullptr;

    return At(Index)->As<C>();
}
// DATALIST

void *DataList::operator[](int32_t Index) const
{
    if (Index >= Length || -Index > Length)
        return nullptr;

    if (Index < 0) // From end
        Index = Length + Index;

    if (Type[Index] == Types::ID)
    {
        if (Objects.IsValid(*At<IDClass>(Index)) == false)
            return nullptr;

        return Objects.At(*At<IDClass>(Index))->Values[At<IDClass>(Index)->ValueIndex()];
    }
    else
        return Data[Index];
};

Types DataList::TypeAt(int32_t Index) const // Returns address or nullptr if invalid
{
    if (IsValid(Index) == false)
        return Types::Undefined;

    if (Index < 0) // From end
        Index = Length + Index;

    if (Type[Index] == Types::ID)
    {
        if (Objects.IsValid(*At<IDClass>(Index)) == false)
            return Types::Undefined;

        return Objects.At(*At<IDClass>(Index))->Values.TypeAt(At<IDClass>(Index)->ValueIndex());
    }
    else
        return Type[Index];
};