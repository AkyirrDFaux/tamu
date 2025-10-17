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
            Data = Data << ByteArray((char*)&Type, sizeof(Types));
        else
            Data = Data << ByteArray((char*)&Type, sizeof(Types)) << ByteArray((char*)Values.Data[Index], GetValueSize(Type));
    }
    return Data;
};

bool BaseClass::SetValue(ByteArray &Input)
{
    if (Input.Type() != Type)
    {
        return false;
        ReportError(Status::InvalidType);
    }

    else if (GetValueSize(Type) == 0)
        return true;

    // INCOMPLETE, needs raw data pointer
    if (Type == Types::Text)
        *ValueAs<String>() = Input.As<String>();
    else if (GetValueSize(Type) == sizeof(uint8_t))
        *ValueAs<uint8_t>() = Input.As<uint8_t>();
    else if (GetValueSize(Type) == sizeof(uint32_t))
        *ValueAs<uint32_t>() = Input.As<uint32_t>();
    else if (GetValueSize(Type) == sizeof(Vector2D))
        *ValueAs<Vector2D>() = Input.As<Vector2D>();
    else if (GetValueSize(Type) == sizeof(Coord2D))
        *ValueAs<Coord2D>() = Input.As<Coord2D>();
    else
        return false;

    Setup();
    return true;
}