// BaseClass *CreateObject(ObjectTypes Type, bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);

template <>
ByteArray::ByteArray(const BaseClass &Data)
{
    // 1. Find the ARRAY POSITION of this object
    int32_t ArrayPos = Objects.Search(const_cast<BaseClass *>(&Data));

    uint8_t idBytes[3] = {0, 0, 0};

    if (ArrayPos != -1) {
        // 2. Get the ACTUAL stored ID Key from the Register
        uint16_t ActualID = Objects.Object[ArrayPos].Index; 
        
        idBytes[0] = 0;                     // Net ID (still hardcoded 0 for now)
        idBytes[1] = (uint8_t)(ActualID >> 8);  // Actual Group ID
        idBytes[2] = (uint8_t)(ActualID & 0xFF); // Actual Device ID
    }

    // 3. Construct the array as before...
    *this = ByteArray((const char *)idBytes, 3);
    HeaderArray[0].Type = Types::Reference;
    
    *this = *this << ByteArray(Data.Type)
                  << ByteArray(Data.Flags)
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