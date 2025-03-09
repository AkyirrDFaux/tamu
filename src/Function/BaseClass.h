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
        if(Modules[Index]->ReferencesCount > 0)
            Modules[Index]->ReferencesCount--;

        if (Modules[Index]->Flags == Flags::Auto) // If modules are static (directly tied) deconstruct them too
            delete Modules[Index];
    }
    // Remove this as a module from all references
    for (int32_t Index = 0; Index < Objects.Allocated; Index++){
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
    ByteArray Data = ByteArray(Type);
    if (Type == Types::Text)
        Data = Data << ByteArray(*ValueAs<String>());
    else if (GetValueSize(Type) == sizeof(uint8_t))
        Data = Data << ByteArray(*ValueAs<uint8_t>());
    else if (GetValueSize(Type) == sizeof(uint32_t))
        Data = Data << ByteArray(*ValueAs<uint32_t>());
    else if (GetValueSize(Type) == sizeof(Vector2D))
        Data = Data << ByteArray(*ValueAs<Vector2D>());
    else if (GetValueSize(Type) == sizeof(Coord2D))
        Data = Data << ByteArray(*ValueAs<Coord2D>());
    else
        return ByteArray();

    return Data;
};

bool BaseClass::SetValue(ByteArray *Input)
{
    if (Input->Type() != Type)
    {
        return false;
        ReportError(Status::InvalidType);
    }

    else if (GetValueSize(Type) == 0)
        return true;

    if (Type == Types::Text)
        *ValueAs<String>() = Input->Data<String>();
    else if (GetValueSize(Type) == sizeof(uint8_t))
        *ValueAs<uint8_t>() = Input->Data<uint8_t>();
    else if (GetValueSize(Type) == sizeof(uint32_t))
        *ValueAs<uint32_t>() = Input->Data<uint32_t>();
    else if (GetValueSize(Type) == sizeof(Vector2D))
        *ValueAs<Vector2D>() = Input->Data<Vector2D>();
    else if (GetValueSize(Type) == sizeof(Coord2D))
        *ValueAs<Coord2D>() = Input->Data<Coord2D>();
    else
        return false;

    Setup();
    return true;
}