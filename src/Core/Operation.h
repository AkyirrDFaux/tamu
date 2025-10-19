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
    void *Target = Values[1];
    void *Value = Modules[0]->Values[Modules.IDs[0].Sub() - 1];

    if (Target == nullptr || Value == nullptr)
        return true;

    if (Values.Type[1] != Modules[0]->Values.Type[Modules.IDs[0].Sub() - 1])
        return true;

    memcpy(Value, Target, GetValueSize(Values.Type[1]));
    return true;
}

bool Operation::Extract()
{
    // (ID)Value[1]  (Value(2), ...) -> Module[0]
    void *Target = Modules[0]->Values[Modules.IDs[0].Sub() - 1];
    void *Value = Objects.Find(*Values.At<IDClass>(1))->Values[Values.At<IDClass>(1)->Sub() - 1]; // From values[1] ID

    if (Target == nullptr || Value == nullptr)
        return true;

    if (Modules[0]->Values.Type[Modules.IDs[0].Sub() - 1] == Types::Vector2D && Objects.Find(*Values.At<IDClass>(1))->Values.Type[Values.At<IDClass>(1)->Sub() - 1] == Types::Vector3D)
        *(Vector2D *)Target = Vector2D(((Vector3D *)Value)->GetByIndex(*Values.At<uint8_t>(2)), ((Vector3D *)Value)->GetByIndex(*Values.At<uint8_t>(3)));

    return true;
}

bool Operation::Combine()
{
    void *Target = nullptr;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
    {
        Target = Modules[Index]->Values[Modules.IDs[Index].Sub() - 1];
        if (Target == nullptr)
            return true;

        if (Modules[Index]->Values.Type[Modules.IDs[Index].Sub() - 1] == Types::Coord2D && Values.Type[1] == Types::Vector2D && Values.Type[2] == Types::Number)
            *(Coord2D *)Target = Coord2D(*Values.At<Vector2D>(1), Vector2D(*Values.At<float>(2)));
    }
    return true;
}

bool Operation::Add()
{
    if (Values.Type[1] != Values.Type[2])
        return true;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
    {
        if (Values.Type[1] == Types::Vector2D)
            *Modules[0]->Values.At<Vector2D>(Modules.IDs[0].Sub() - 1) = *Values.At<Vector2D>(1) + *Values.At<Vector2D>(2);
    }

    return true;
}

bool Operation::MoveTo()
{
    void *Target = Values[1];
    uint32_t *Time = Values.At<uint32_t>(2);
    void *Value = Modules[0]->Values[Modules.IDs[0].Sub() - 1];

    if (Target == nullptr || Time == nullptr || Value == nullptr)
        return true;

    if (Values.Type[1] != Modules[0]->Values.Type[Modules.IDs[0].Sub() - 1])
        return true;

    if (Values.Type[1] == Types::Coord2D)
        *(Coord2D *)Value = ((Coord2D *)Value)->TimeMove(*(Coord2D *)Target, *Time);
    else if (Values.Type[1] == Types::Number)
        *(float *)Value = TimeMove(*(float *)Value, *(float *)Target, *Time);
    else
        return true;

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