template <class C>
bool Variable<C>::Run()
{
    if (Modules.IsValid(0,Types::Operation) == false) // No operation
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

template <>
bool Variable<float>::Run()
{
    if (Modules.IsValid(0,Types::Operation) == false) // No operation
        return true;

    switch (*Modules.GetValue<Operations>(0))
    {
    case Operations::Equal: // Inverted hierarchy! Writes to modules instead of itself
        for (int32_t Index = Modules.FirstValid(Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Type))
            *Modules.GetValue<float>(Index) = *Data;
        return true;
    case Operations::MoveTo: // 1 Target, 2 Time
        if (Modules.IsValid(1,Types::Number) == false || Modules.IsValid(2,Types::Time) == false)
            return true;

        *Data = TimeMove(*Data, *Modules.GetValue<float>(1), *Modules.GetValue<uint32_t>(2));
        return (CurrentTime >= *Modules.GetValue<uint32_t>(2));
    default:
        ReportError(Status::InvalidValue, "Operation not implemeted");
        return true;
    }
    return true;
}

template <>
bool Variable<Coord2D>::Run()
{
    if (Modules.IsValid(0,Types::Operation) == false) // No operation
        return true;

    switch (*Modules.GetValue<Operations>(0))
    {
    case Operations::Equal: // Inverted hierarchy! Writes to modules instead of itself
        for (int32_t Index = Modules.FirstValid(Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Type))
            *Modules.GetValue<Coord2D>(Index) = *Data;
        return true;
    case Operations::MoveTo: // 1 Target, 2 Time
        if (Modules.IsValid(1,Types::Coord2D) == false || Modules.IsValid(2,Types::Time) == false)
            return true;

        *Data = Data->TimeMove(*Modules.GetValue<Coord2D>(1), *Modules.GetValue<uint32_t>(2));
        return (CurrentTime >= *Modules.GetValue<uint32_t>(2));
    default:
        ReportError(Status::InvalidValue, "Operation not implemeted");
        return true;
    }
    return true;
}
template <>
bool Variable<uint32_t>::Run()
{
    if (Modules.IsValid(0,Types::Operation) == false) // No operation
        return true;

    switch (*Modules.GetValue<Operations>(0))
    {
    case Operations::Equal: // Inverted hierarchy! Writes to modules instead of itself
        for (int32_t Index = Modules.FirstValid(Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Type))
            *Modules.GetValue<uint32_t>(Index) = *Data;
        return true;
    case Operations::AddDelay:
        if (Modules.IsValid(1,Types::Time) == false)
            return true;

        *Data = *Modules.GetValue<uint32_t>(1) + CurrentTime;
        return true;
    case Operations::Delay: // Data is start time, Module 1 is delay
        if (Modules.IsValid(1,Types::Time) == false)
            return true;

        if (*Data == 0)
            *Data = CurrentTime;
        else if (CurrentTime > *Data + *Modules.GetValue<uint32_t>(1))
        {
            *Data = 0; //Reset
            return true; //Finished
        }
        return false;
    default:
        ReportError(Status::InvalidValue, "Operation not implemeted");
        return true;
    }
    return true;
}

/*
case Operations::Add:
        if (!Modules.IsValid(1) || Type != Modules[2]->Type)
            return true;

        *Data = 0;
        for (int32_t Index = Modules.FirstValid(Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Type))
            *Data += *Modules.GetValue<C>(Index);
        return true;
*/