class Operation : public BaseClass
{
public:
    ByteArray Temp;

    Operation(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    bool Run();
    bool RunPart(int32_t Start);                  // Start = operation value index
    int32_t OperationNumber(int32_t Start) const; // Number for temp list
    int32_t ParameterNumber(int32_t Start) const; // Length of operation value space

    // TODO
    Types TypeThrough(int32_t Index); // Type through ID, runs functions
    template <class C>
    C GetThrough(int32_t Index) const; // Get through ID, Assumes it's valid, and type is checked

private:
    bool Equal(int32_t Start);
    bool Extract(int32_t Start);
    bool Combine(int32_t Start);
    bool Add(int32_t Start);
    bool Multiply(int32_t Start);
    bool MoveTo(int32_t Start);
    bool Delay(int32_t Start);
    bool AddDelay(int32_t Start);
    bool SetFlags(int32_t Start);
    bool ResetFlags(int32_t Start);
    bool Sine(int32_t Start);
};

Operation::Operation(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    Type = ObjectTypes::Operation;
    Name = "Operation";

    ValueSet(Operations::None);
}

bool Operation::Run()
{
    bool Done = RunPart(0);
    Types Type = Temp.Type(0);

    if (Type == Types::Undefined)
        return Done;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
    {
        if (Modules.ValueTypeAt(Index) != Type)
            continue;

        switch (Type)
        {
        case Types::Number:
            Modules.ValueSet(Temp.Get<Number>(0), Index);
            break;
        case Types::Time:
            Modules.ValueSet(Temp.Get<uint32_t>(0), Index);
            break;
        case Types::Vector2D:
            Modules.ValueSet(Temp.Get<Vector2D>(0), Index);
            break;
        case Types::Vector3D:
            Modules.ValueSet(Temp.Get<Vector3D>(0), Index);
            break;
        case Types::Coord2D:
            Modules.ValueSet(Temp.Get<Coord2D>(0), Index);
            break;
        default:
            ReportError(Status::InvalidValue, "Operation output not implemeted" + String((uint8_t)Type));
            break;
        }
    }

    return Done;
}

bool Operation::RunPart(int32_t Start)
{
    if (Values.Type(Start) != Types::Operation) // No operation
        return true;

    switch (ValueGet<Operations>(Start))
    {
    case Operations::Equal:
        return Equal(Start);
    case Operations::Extract:
        return Extract(Start);
    case Operations::Combine:
        return Combine(Start);
    case Operations::Add:
        return Add(Start);
    case Operations::Multiply:
        return Multiply(Start);
    case Operations::MoveTo:
        return MoveTo(Start);
    case Operations::Delay:
        return Delay(Start);
    case Operations::AddDelay:
        return AddDelay(Start);
    case Operations::SetFlags:
        return SetFlags(Start);
    case Operations::ResetFlags:
        return ResetFlags(Start);
    case Operations::Sine:
        return Sine(Start);
    default:
        ReportError(Status::InvalidValue, "Operation not implemeted" + String((uint8_t)Type));
        return true;
    }
    return true;
}

int32_t Operation::OperationNumber(int32_t Start) const
{
    int32_t Number = 0;
    for (int32_t Index = 0; Index < Values.Length; Index++)
    {
        if (Index >= Start)
            break;
        if (Values.Type(Index) == Types::Operation)
            Number++;
    }
    return Number;
};

int32_t Operation::ParameterNumber(int32_t Start) const
{
    int32_t Number = 0;
    for (int32_t Index = Start + 1; Index < Values.Length; Index++)
    {
        if (Values.Type(Index) == Types::Operation)
            break;
        Number++;
    }
    return Number;
};

Types Operation::TypeThrough(int32_t Index)
{
    Types Type = Values.Type(Index);
    if (Type == Types::ID)
    {
        IDClass ID = Values.Get<IDClass>(Index);
        if (ID.Base() == 0) // Local reference
        {
            int32_t Ref = ID.ValueIndex();
            Type = Values.Type(Ref);
            if (Type == Types::Operation)
            {
                RunPart(Ref);
                Type = Temp.Type(OperationNumber(Ref));
            }
        }
        else
            return Objects.ValueTypeAt(ID);
    }
    return Type;
};
template <class C>
C Operation::GetThrough(int32_t Index) const
{
    Types Type = Values.Type(Index);
    if (Type == Types::ID)
    {
        IDClass ID = Values.Get<IDClass>(Index);
        if (ID.Base() == 0) // Local reference
        {
            int32_t Ref = ID.ValueIndex();
            Type = Values.Type(Ref);
            if (Type == Types::Operation)
                return Temp.Get<C>(OperationNumber(Ref));
            else
                return ValueGet<C>(Ref);
        }
        else
            return Objects.ValueGet<C>(ID);
    }
    return ValueGet<C>(Index);
};

bool Operation::Equal(int32_t Start)
{
    // REPLACE and add fn. if equal is main op. -> num in == num out => one by one / otherwise copy first to all
    Temp.Copy(Values, 1, 0);
    return true;
}

bool Operation::Extract(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    for (int32_t Index = 0; Index < InputNumber; Index++)
        InputTypes[Index] = TypeThrough(Start + 1 + Index);

    if (InputNumber >= 3 &&
        InputTypes[0] == Types::Vector3D &&
        InputTypes[1] == Types::Byte &&
        InputTypes[2] == Types::Byte)
    {
        Vector3D Source = GetThrough<Vector3D>(Start + 1); // Needs get with IDs
        uint8_t A = GetThrough<uint8_t>(Start + 2);
        uint8_t B = GetThrough<uint8_t>(Start + 3);
        Temp.Set<Vector2D>(Source.GetByIndex(A, B), OperationNumber(Start));
    }

    return true;
}

bool Operation::Combine(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    for (int32_t Index = 0; Index < InputNumber; Index++)
        InputTypes[Index] = TypeThrough(Start + 1 + Index);

    if (InputNumber >= 2 &&
        InputTypes[0] == Types::Vector2D &&
        InputTypes[1] == Types::Number)
    {
        Vector2D A = GetThrough<Vector2D>(Start + 1);
        Number B = GetThrough<Number>(Start + 2);
        Temp.Set<Coord2D>(Coord2D(A, B), OperationNumber(Start));
    }

    return true;
}

bool Operation::Add(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    for (int32_t Index = 0; Index < InputNumber; Index++)
        InputTypes[Index] = TypeThrough(Start + 1 + Index);

    if (InputNumber >= 2 &&
        InputTypes[0] == Types::Vector2D &&
        InputTypes[1] == Types::Vector2D)
    {
        Vector2D A = GetThrough<Vector2D>(Start + 1);
        Vector2D B = GetThrough<Vector2D>(Start + 2);
        Temp.Set<Vector2D>(A + B, OperationNumber(Start));
    }

    return true;
}

bool Operation::Multiply(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    for (int32_t Index = 0; Index < InputNumber; Index++)
        InputTypes[Index] = TypeThrough(Start + 1 + Index);

    if (InputNumber >= 2 &&
        InputTypes[0] == Types::Vector2D &&
        InputTypes[1] == Types::Vector2D)
    {
        Vector2D A = GetThrough<Vector2D>(Start + 1);
        Vector2D B = GetThrough<Vector2D>(Start + 2);
        Temp.Set<Vector2D>(A * B, OperationNumber(Start));
    }
    if (InputNumber >= 2 &&
        InputTypes[0] == Types::Number &&
        InputTypes[1] == Types::Number)
    {
        Number A = GetThrough<Number>(Start + 1);
        Number B = GetThrough<Number>(Start + 2);
        Temp.Set<Number>(A * B, OperationNumber(Start));
    }
    return true;
}

bool Operation::MoveTo(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    for (int32_t Index = 0; Index < InputNumber; Index++)
        InputTypes[Index] = TypeThrough(Start + 1 + Index);

    uint32_t Time;
    if (InputNumber >= 2 &&
        InputTypes[0] == Types::Vector2D &&
        InputTypes[1] == Types::Time)
    {
        Vector2D Target = GetThrough<Vector2D>(Start + 1);
        Time = GetThrough<uint32_t>(Start + 2);
        Vector2D Value;
        for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        {
            if (Modules.ValueTypeAt(Index) != Types::Vector2D)
                continue;
            Value = Modules.ValueGet<Vector2D>(Index);
            Modules.ValueSet<Vector2D>(Value.TimeMove(Target, Time), Index);
        }
    }
    else if (InputNumber >= 2 &&
             InputTypes[0] == Types::Coord2D &&
             InputTypes[1] == Types::Time)
    {
        Coord2D Target = GetThrough<Coord2D>(Start + 1);
        Time = GetThrough<uint32_t>(Start + 2);
        Coord2D Value;
        for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        {
            if (Modules.ValueTypeAt(Index) != Types::Coord2D)
                continue;
            Value = Modules.ValueGet<Coord2D>(Index);
            Modules.ValueSet<Coord2D>(Value.TimeMove(Target, Time), Index);
        }
    }
    else if (InputNumber >= 2 &&
             InputTypes[0] == Types::Number &&
             InputTypes[1] == Types::Time)
    {
        Number Target = GetThrough<Number>(Start + 1);
        Time = GetThrough<uint32_t>(Start + 2);
        Number Value;
        for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        {
            if (Modules.ValueTypeAt(Index) != Types::Number)
                continue;
            Value = Modules.ValueGet<Number>(Index);
            Modules.ValueSet<Number>(TimeMove(Value, Target, Time), Index);
        }
    }
    else
        return true;
    return (CurrentTime >= Time);
}

bool Operation::Delay(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    for (int32_t Index = 0; Index < InputNumber; Index++)
        InputTypes[Index] = TypeThrough(Start + 1 + Index);

    // 0 = delay, 1 = timer
    if (InputNumber < 2 || InputTypes[0] != Types::Time || InputTypes[1] != Types::Time) // Values not avaliable
        return true;

    uint32_t Timer = Values.Get<uint32_t>(Start + 2);

    if (Timer == 0)
        Values.Set<uint32_t>(CurrentTime, Start + 2);
    else if (CurrentTime > Timer + GetThrough<uint32_t>(Start + 1))
    {
        Values.Set<uint32_t>(0, Start + 2); // Reset
        return true;                        // Finished
    }
    return false;
}

bool Operation::AddDelay(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    for (int32_t Index = 0; Index < InputNumber; Index++)
        InputTypes[Index] = TypeThrough(Start + 1 + Index);

    if (InputNumber < 1 || InputTypes[0] != Types::Time)
        return true;

    Temp.Set<uint32_t>(GetThrough<uint32_t>(Start + 1) + CurrentTime, OperationNumber(Start));

    return true;
}

bool Operation::SetFlags(int32_t Start)
{
    if (Values.Type(1) != Types::Flags)
        return true;

    FlagClass Value = ValueGet<FlagClass>(1);

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        Modules.At(Index)->Flags += Value.Values;

    return true;
}

bool Operation::ResetFlags(int32_t Start)
{
    if (Values.Type(1) != Types::Flags)
        return true;

    FlagClass Value = ValueGet<FlagClass>(1);

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        Modules.At(Index)->Flags -= Value.Values;

    return true;
}

bool Operation::Sine(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    for (int32_t Index = 0; Index < InputNumber; Index++)
        InputTypes[Index] = TypeThrough(Start + 1 + Index);

    // 0m = X, 1 = Multiplier, 2 = Phase

    if (InputNumber >= 3 &&
        InputTypes[0] == Types::Time &&
        InputTypes[1] == Types::Number &&
        InputTypes[2] == Types::Number)
    {
        uint32_t A = GetThrough<uint32_t>(Start + 1);
        Number B = GetThrough<Number>(Start + 2);
        Number C = GetThrough<Number>(Start + 3);
        Temp.Set<Number>(sin(A + C * B), OperationNumber(Start));
    }

    return true;
}