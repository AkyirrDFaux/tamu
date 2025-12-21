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
    bool Extract();
    bool Combine();
    bool Add();
    bool Multiply();
    bool MoveTo();
    bool Delay();
    bool AddDelay();
    bool SetFlags();
    bool ResetFlags();
    bool Sine();
};

Operation::Operation(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    Type = ObjectTypes::Operation;
    Name = "Operation";

    Values.Add(Operations::None);
}

bool Operation::Run()
{
    if (Values.IsValid(OperationType, Types::Operation) == false) // No operation
        return true;

    switch (*Values.At<Operations>(OperationType))
    {
    case Operations::Equal:
        return Equal();
    case Operations::Extract:
        return Extract();
    case Operations::Combine:
        return Combine();
    case Operations::Add:
        return Add();
    case Operations::Multiply:
        return Multiply();
    case Operations::MoveTo:
        return MoveTo();
    case Operations::Delay:
        return Delay();
    case Operations::AddDelay:
        return AddDelay();
    case Operations::SetFlags:
        return SetFlags();
    case Operations::ResetFlags:
        return ResetFlags();
    case Operations::Sine:
        return Sine();
    default:
        ReportError(Status::InvalidValue, "Operation not implemeted" + String((uint8_t)Type));
        return true;
    }
    return true;
}

bool Operation::Equal()
{
    void *Target = Values[1];
    void *Value = Modules.ValueAt(0);

    if (Target == nullptr || Value == nullptr)
        return true;

    if (Values.TypeAt(1) != Modules.ValueTypeAt(0))
        return true;

    memcpy(Value, Target, GetDataSize(Values.TypeAt(1)));
    return true;
}

bool Operation::Extract()
{
    // Value[1]  (Value(2), ...) -> Module[0]
    void *Target = nullptr;
    void *Value = Values[1];

    if (Value == nullptr)
        return true;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
    {
        Target = Modules.ValueAt(Index);

        if (Modules.ValueTypeAt(Index) == Types::Vector2D && Values.TypeAt(1) == Types::Vector3D && Values.TypeAt(2) == Types::Byte && Values.TypeAt(3) == Types::Byte)
            *(Vector2D *)Target = Vector2D(((Vector3D *)Value)->GetByIndex(*Values.At<uint8_t>(2)), ((Vector3D *)Value)->GetByIndex(*Values.At<uint8_t>(3)));
    }
    return true;
}

bool Operation::Combine()
{
    void *Target = nullptr;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
    {
        Target = Modules.ValueAt(Index);
        if (Target == nullptr)
            return true;

        if (Modules.ValueTypeAt(Index) == Types::Coord2D && Values.TypeAt(1) == Types::Vector2D && Values.TypeAt(2) == Types::Number)
            *(Coord2D *)Target = Coord2D(*Values.At<Vector2D>(1), Vector2D(*Values.At<Number>(2)));
    }
    return true;
}

bool Operation::Add()
{
    if (Values.TypeAt(1) != Values.TypeAt(2))
        return true;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
    {
        if (Values.TypeAt(1) == Types::Vector2D && Modules.ValueTypeAt(Index) == Types::Vector2D)
            *(Vector2D *)Modules.ValueAt(Index) = *Values.At<Vector2D>(1) + *Values.At<Vector2D>(2);
    }

    return true;
}

bool Operation::Multiply()
{
    if (Values.TypeAt(1) != Values.TypeAt(2))
        return true;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
    {
        if (Values.TypeAt(1) == Types::Vector2D && Modules.ValueTypeAt(Index) == Types::Vector2D)
            *(Vector2D *)Modules.ValueAt(Index) = (*Values.At<Vector2D>(1)) * (*Values.At<Vector2D>(2));
        if (Values.TypeAt(1) == Types::Number && Modules.ValueTypeAt(Index) == Types::Number)
            *(Number *)Modules.ValueAt(Index) = (*Values.At<Number>(1)) * (*Values.At<Number>(2));
    }

    return true;
}

bool Operation::MoveTo()
{
    void *Target = Values[1];
    uint32_t *Time = Values.At<uint32_t>(2);
    void *Value = nullptr;

    if (Target == nullptr || Time == nullptr)
        return true;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
    {
        if (Values.TypeAt(1) != Modules.ValueTypeAt(Index))
            continue;
        Value = Modules.ValueAt(Index);

        if (Values.TypeAt(1) == Types::Coord2D)
            *(Coord2D *)Value = ((Coord2D *)Value)->TimeMove(*(Coord2D *)Target, *Time);
        else if (Values.TypeAt(1) == Types::Number)
            *(Number *)Value = TimeMove(*(Number *)Value, *(Number *)Target, *Time);
    }

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
    uint32_t *Value = (uint32_t *)Modules.ValueAt(0);

    if (Delay == nullptr || Value == nullptr || Modules.ValueTypeAt(0) != Types::Time)
        return true;

    *Value = *Delay + CurrentTime;
    return true;
}

bool Operation::SetFlags()
{
    FlagClass *Value = Values.At<FlagClass>(1);

    if (Value == nullptr)
        return true;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        Modules.At(Index)->Flags += Value->Values;

    return true;
}

bool Operation::ResetFlags()
{
    FlagClass *Value = Values.At<FlagClass>(1);

    if (Value == nullptr)
        return true;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        Modules.At(Index)->Flags -= Value->Values;

    return true;
}

bool Operation::Sine()
{
    uint32_t *Input = Values.At<uint32_t>(1);
    Number *Multiplier = Values.At<Number>(2);
    Number *Phase = Values.At<Number>(3);
    void *Value = nullptr;

    if (Input == nullptr || Multiplier == nullptr || Phase == nullptr)
        return true;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
    {
        Value = Modules.ValueAt(Index);
        if (Value == nullptr)
            continue;
        if (Modules.ValueTypeAt(Index) == Types::Number)
            *(Number *)Value = sin((*Input) * (*Multiplier) + (*Phase));
    }

    return true;
}