BaseClass *CreateObject(ObjectTypes Type, bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);

template <>
ByteArray::ByteArray(const BaseClass &Data)
{
    // Type, ID, Flags, Name, Modules,(+ values)
    *this = ByteArray(Data.Type) << ByteArray(Data.ID) << ByteArray(Data.Flags) << ByteArray(Data.Name) << ByteArray(Data.Modules) << Data.OutputValues();
};

String BaseClass::ContentDebug()
{
    String Text = "ID: ";
    Text += ID.ToString();
    Text += ", Type: ";
    Text += String((uint8_t)Type);
    Text += " , Name: ";
    Text += Name + '\n';

    return Text;
};

BaseClass::BaseClass(IDClass NewID, FlagClass NewFlags)
{
    Flags = NewFlags;
    Objects.Register(this, NewID);
};

BaseClass::~BaseClass()
{
    // Remove references to this from all modules
    for (uint32_t Index = Modules.FirstValid(); Index < Modules.Length; Modules.Iterate(&Index))
    {
        if (Modules[Index]->Flags == Flags::Auto) // If modules are static (directly tied) deconstruct them too
            delete Modules[Index];
    }
    // Remove this as a module from all references
    for (uint32_t Index = 0; Index < Objects.Allocated; Index++)
    {
        if (Objects.IsValid(Index) == false)
            continue;
        Objects[Index]->Modules.Remove(this);
    }

    // Clear lists, unregister this
    Modules.RemoveAll();
    Objects.Unregister(this);
};

template <class C>
C BaseClass::ValueGet(int32_t Index) const
{
    return Values.Get<C>(Index);
}

template <class C>
bool BaseClass::ValueSet(C Value, int32_t Index)
{
    Values.Set(Value, Index);
    Setup(Index);
    return true;
}

ByteArray BaseClass::OutputValues(int32_t Value) const
{
    if (Value == 0)
        return Values;

    Value--; //ID to ValueIndex
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

    Value--; //ID to ValueIndex
    Values.Copy(Input, Index, Value);
    Setup(Value);
    return true;
}