class Operation : public BaseClass
{
public:
    enum Value
    {
        OperationType
    };

    Operation(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    bool Run();

private:
    bool Equal();
    bool MoveTo();
    bool Delay();
    bool AddDelay();
};

Operation::Operation(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    Type = Types::Operation;
    Name = "Operation";

    Values.Add(Operations::None);
}

bool Operation::Run()
{
    if (Values.IsValid(OperationType, Types::Operation) == false) // No operation
        return true;

    Serial.println((uint8_t)*Values.At<Operations>(OperationType));

    switch (*Values.At<Operations>(OperationType))
    {
    case Operations::Equal:
        return Equal();
    case Operations::MoveTo:
        return MoveTo();
    case Operations::Delay:
        return Delay();
    case Operations::AddDelay:
        return AddDelay();
    default:
        ReportError(Status::InvalidValue, "Operation not implemeted" + String((uint8_t)Type));
        return true;
    }
    return true;
}

bool Operation::Equal()
{
    float *Target = Values.At<float>(1);
    float *Value = Modules[0]->Values.At<float>(Modules.IDs[0].Sub() - 1); // Proprietary

    if (Target == nullptr || Value == nullptr)
        return true;

    *Value = *Target;
    return true;
}

bool Operation::MoveTo()
{
    float *Target = Values.At<float>(1);
    uint32_t *Time = Values.At<uint32_t>(2);

    float *Value = Modules[0]->Values.At<float>(Modules.IDs[0].Sub() - 1); // Proprietary

    if (Target == nullptr || Time == nullptr || Value == nullptr)
        return true;

    *Value = TimeMove(*Value, *Target, *Time);
    return (CurrentTime >= *Time);
}

bool Operation::Delay()
{
    uint32_t *Delay = Values.At<uint32_t>(1);
    uint32_t *Timer = Values.At<uint32_t>(2);

    if (Delay == nullptr || Timer == nullptr) // Values not avaliable
        return true;

    if (*Timer == 0)
        *Timer = CurrentTime;
    else if (CurrentTime > *Timer + *Delay)
    {
        *Timer = 0;  // Reset
        return true; // Finished
    }
    return false;
}

bool Operation::AddDelay()
{
    uint32_t *Delay = Values.At<uint32_t>(1);

    uint32_t *Value = Modules[0]->Values.At<uint32_t>(Modules.IDs[0].Sub() - 1); // Proprietary
    if (Delay == nullptr || Value == nullptr)
        return true;

    *Value = *Delay + CurrentTime;
    return true;
}

/*
template <>
bool Variable<float>::Run()
{
    if (Modules.IsValid(0, Types::Operation) == false) // No operation
        return true;

    switch (*Modules.GetValue<Operations>(0))
    {
    case Operations::Equal: // Inverted hierarchy! Writes to modules instead of itself
        for (int32_t Index = Modules.FirstValid(Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Type))
            *Modules.GetValue<float>(Index) = *Data;
        return true;
    case Operations::MoveTo: // 1 Target, 2 Time
        if (Modules.IsValid(1, Types::Number) == false || Modules.IsValid(2, Types::Time) == false)
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
    if (Modules.IsValid(0, Types::Operation) == false) // No operation
        return true;

    switch (*Modules.GetValue<Operations>(0))
    {
    case Operations::Equal: // Inverted hierarchy! Writes to modules instead of itself
        for (int32_t Index = Modules.FirstValid(Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Type))
            *Modules.GetValue<Coord2D>(Index) = *Data;
        return true;
    case Operations::MoveTo: // 1 Target, 2 Time
        if (Modules.IsValid(1, Types::Coord2D) == false || Modules.IsValid(2, Types::Time) == false)
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
    if (Modules.IsValid(0, Types::Operation) == false) // No operation
        return true;

    switch (*Modules.GetValue<Operations>(0))
    {
    case Operations::Equal: // Inverted hierarchy! Writes to modules instead of itself
        for (int32_t Index = Modules.FirstValid(Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Type))
            *Modules.GetValue<uint32_t>(Index) = *Data;
        return true;
    case Operations::AddDelay:
        if (Modules.IsValid(1, Types::Time) == false)
            return true;

        *Data = *Modules.GetValue<uint32_t>(1) + CurrentTime;
        return true;
    case Operations::Delay: // Data is start time, Module 1 is delay
        if (Modules.IsValid(1, Types::Time) == false)
            return true;

        if (*Data == 0)
            *Data = CurrentTime;
        else if (CurrentTime > *Data + *Modules.GetValue<uint32_t>(1))
        {
            *Data = 0;   // Reset
            return true; // Finished
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
