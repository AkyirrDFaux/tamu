BaseClass::BaseClass(IDClass NewID, FlagClass NewFlags)
{
    Flags = NewFlags;
    Objects.Register(this, NewID);
};

BaseClass::~BaseClass()
{
    // Remove references to this from all modules
    for (int32_t Index = Modules.FirstValid(); Index < Modules.Length; Modules.Iterate(&Index))
    {
        if (Modules[Index]->ReferencesCount > 0)
            Modules[Index]->ReferencesCount--;

        if (Modules[Index]->Flags == Flags::Auto) // If modules are static (directly tied) deconstruct them too
            delete Modules[Index];
    }
    // Remove this as a module from all references
    for (int32_t Index = 0; Index < Objects.Allocated; Index++)
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
C *BaseClass::ValueAs() const
{
    return As<Variable<C>>()->Data;
};

ByteArray BaseClass::GetValue() const
{
    ByteArray Data = ByteArray();
    Types Type = Types::Undefined;
    for (uint32_t Index = 0; Index < Values.Length; Index++)
    {
        Type = Values.Type[Index];
        if (Type == Types::Text)
            Data = Data << ByteArray(*Values.At<String>(Index));
        else if (Type == Types::Undefined)
            Data = Data << ByteArray((char *)&Type, sizeof(Types));
        else
            Data = Data << ByteArray((char *)&Type, sizeof(Types)) << ByteArray((char *)Values.Data[Index], GetValueSize(Type));
    }
    return Data;
};

//Not sure this is working
bool BaseClass::SetValue(ByteArray &Input)
{
    ByteArray Part = Input.ExtractPart();
    int32_t Index = 0;
    while (Part.Length > 0)
    {
        if (Part.Type() == Types::Undefined)
        {
            Part = Input.ExtractPart();
            Index++;
            continue;
        }

        
        if (Values.IsValid(Index) == false || Values.Type[Index] != Part.Type()) // Prepare for copying
        {
            if (Values.IsValid(Index)) // Already occupied, but different type
                Values.Delete(Index);
            else //Possibly not allocated yet
                Values.Expand(Index + 1);

            Values.Data[Index] = new char[Part.SizeOfData()]; 
            Values.Type[Index] = Part.Type();
        }

        memcpy(Values.Data[Index],Part.Array + sizeof(Types), Part.SizeOfData());

        Part = Input.ExtractPart();
        Index++;
    }
    Setup();
    return true;
}