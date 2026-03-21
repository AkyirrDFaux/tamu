// BaseClass *CreateObject(ObjectTypes Type, bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);

template <>
ByteArray::ByteArray(const BaseClass &Data)
{
    int32_t Index = Objects.Search(&Data);

    *this = ByteArray(Reference(0,Objects.Object[Index].GroupID, Objects.Object[Index].DeviceID));
    *this = *this << ByteArray(Data.Type)
                  << ByteArray(Objects.Object[Index].Info)
                  << ByteArray(Data.Name)
                  << Data.Values;
}

template <class C>
Getter<C> BaseClass::ValueGet(const Path &Location) const
{
    // Direct passthrough to the internal ByteArray
    return Values.Get<C>(Location);
}

template <class C>
bool BaseClass::ValueSet(C Value, const Path &Location)
{
    // 1. Update the internal data tree
    Values.Set(Value, Location);

    // 2. Trigger any hardware or logic setup tied to this specific path
    Setup(Location);

    return true;
}

/*ByteArray BaseClass::OutputValues(int32_t Value) const
{
    if (Value == 0)
        return Values;

    Value--; // ID to ValueIndex
    int32_t Start = Values.GetStart(Value);
    int32_t End = Values.GetStart(Value + 1);

    if (Start < 0)
        return ByteArray();
    else if (End < 0)
        return Values.SubArray(Start);
    else
        return Values.SubArray(Start, End - Start);
};

bool BaseClass::InputValues(ByteArray &Input, int32_t Index, uint8_t Value)
{
    // Value == 0 is everything, > 0 only one specified
    if (Value == 0)
    {
        Values = Input.SubArray(Input.GetStart(Index));
        Setup(-1);
        return true;
    }

    Value--; // ID to ValueIndex
    Values.Copy(Input, Index, Value);
    Setup(Value);
    return true;
}*/