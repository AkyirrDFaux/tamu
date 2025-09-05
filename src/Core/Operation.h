template <class C>
bool Variable<C>::Run()
{
    if (Modules.IsValid(0) == false || Modules[0]->Type != Types::Operation) // No operation
        return true;

    switch (*Modules.GetValue<Operations>(0))
    {
    case Operations::Equal: // Inverted hierarchy! Writes to modules instead of itself
        for (int32_t Index = Modules.FirstValid(Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Type))
            *Modules.GetValue<C>(Index) = *Data;
        return true;
    default:
        ReportError(Status::InvalidValue, "Operation not implemeted" + String((uint8_t)Type));
        return true;
    }
    return true;
}
/*
template <class C>
bool Variable<C>::Run()
{
    if (Modules.IsValid(0) == false || Modules[0]->Type != Types::Operation) // No operation
        return true;

    switch (*Modules.GetValue<Operations>(0))
    {
    case Operations::Equal:
        if (Modules.IsValid(1) == false && Modules[1]->Type != Type)
            return true;

        *Data = *Modules.GetValue<C>(1);
        return true;
    case Operations::Add:
        if (!Modules.IsValid(1) || Type != Modules[2]->Type)
            return true;

        *Data = 0;
        for (int32_t Index = Modules.FirstValid(Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Type))
            *Data += *Modules.GetValue<C>(Index);
        return true;
    case Operations::AddDelay:
        if (Modules.IsValid(1) == false || Type != Types::Time || Modules[1]->Type != Types::Time)
            return true;

        *Data = *Modules.GetValue<C>(1) + CurrentTime;
        return true;
    default:
        ReportError(Status::InvalidValue, "Operation not implemeted");
        return true;
    }
    return true;
}*/

template <>
bool Variable<uint32_t>::Run()
{
    if (Modules.IsValid(0) == false || Modules[0]->Type != Types::Operation) // No operation
        return true;

    switch (*Modules.GetValue<Operations>(0))
    {
    case Operations::Equal: // Inverted hierarchy! Writes to modules instead of itself
        for (int32_t Index = Modules.FirstValid(Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Type))
            *Modules.GetValue<uint32_t>(Index) = *Data;
        return true;
    case Operations::AddDelay:
        if (Modules.IsValid(1) == false || Type != Types::Time || Modules[1]->Type != Types::Time)
            return true;

        *Data = *Modules.GetValue<uint32_t>(1) + CurrentTime;
        return true;
    default:
        ReportError(Status::InvalidValue, "Operation not implemeted");
        return true;
    }
    return true;
}