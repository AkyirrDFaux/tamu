class Operation : public BaseClass
{
public:
    DataList Temp;

    Operation(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    bool Run();
    bool RunPart(int32_t Start);                                                     // Start = operation value index
    int32_t OperationNumber(int32_t Start);                                          // Number for temp list
    int32_t ParameterNumber(int32_t Start);                                          // Length of operation value space
    bool GetInputs(int32_t Start, Types *InputTypes, void **Inputs, int32_t Length); // Gets inputs and pointers to them

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

    Values.Add(Operations::None);
}

bool Operation::Run()
{
    bool Done = RunPart(0);

    if (Temp.IsValid(0) == false)
        return Done;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
    {
        if (Modules.ValueTypeAt(Index) != Temp.TypeAt(0))
            continue;

        switch (Temp.TypeAt(0))
        {
        case Types::Number:
            Modules.ValueSet<Number>(*Temp.At<Number>(0), Index);
            break;
        case Types::Time:
            Modules.ValueSet<uint32_t>(*Temp.At<uint32_t>(0), Index);
            break;
        case Types::Vector2D:
            Modules.ValueSet<Vector2D>(*Temp.At<Vector2D>(0), Index);
            break;
        case Types::Vector3D:
            Modules.ValueSet<Vector3D>(*Temp.At<Vector3D>(0), Index);
            break;
        case Types::Coord2D:
            Modules.ValueSet<Coord2D>(*Temp.At<Coord2D>(0), Index);
            break;
        default:
            ReportError(Status::InvalidValue, "Operation output not implemeted" + String((uint8_t)Temp.TypeAt(0)));
            break;
        }
    }

    return Done;
}

bool Operation::RunPart(int32_t Start)
{
    if (Values.IsValid(Start, Types::Operation) == false) // No operation
        return true;

    switch (*Values.At<Operations>(Start))
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

int32_t Operation::OperationNumber(int32_t Start)
{
    int32_t Number = 0;
    for (int32_t Index = 0; Index < Values.Length; Index++)
    {
        if (Index >= Start)
            break;
        if (Values.IsValid(Index, Types::Operation))
            Number++;
    }
    return Number;
};

int32_t Operation::ParameterNumber(int32_t Start)
{
    int32_t Number = 0;
    for (int32_t Index = Start + 1; Index < Values.Length; Index++)
    {
        if (Values.IsValid(Index, Types::Operation))
            break;
        Number++;
    }
    return Number;
};

bool Operation::GetInputs(int32_t Start, Types *InputTypes, void **Inputs, int32_t Length)
{
    bool Done = true;
    for (int32_t Index = 0; Index < Length; Index++)
    {
        if (Values.IsValid(Index + Start + 1, Types::ID) && Values.At<IDClass>(Index + Start + 1)->Base() == NoID) // Local reference
        {
            int32_t NextStart = Values.At<IDClass>(Index + Start + 1)->ValueIndex();
            Done &= RunPart(NextStart);
            InputTypes[Index] = Temp.TypeAt(OperationNumber(NextStart));
            Inputs[Index] = Values[OperationNumber(NextStart)];
        }
        else
        {
            InputTypes[Index] = Values.TypeAt(Index + Start + 1);
            Inputs[Index] = Values[Index + Start + 1];
        }
    }
    return Done;
};

bool Operation::Equal(int32_t Start)
{
    // Replace and add fn. if equal is main op. -> num in == num out => one by one / otherwise copy first to all
    
    void *Target = Values[1];
    void *Value = Modules.ValueAt(0);

    if (Target == nullptr || Value == nullptr)
        return true;

    if (Values.TypeAt(1) != Modules.ValueTypeAt(0))
        return true;

    memcpy(Value, Target, GetDataSize(Values.TypeAt(1)));
    return true;
}

bool Operation::Extract(int32_t Start)
{
    // Value[1]  (Value(2), ...) -> Module[0]
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    void *Inputs[InputNumber];
    GetInputs(Start, InputTypes, Inputs, InputNumber);

    if (InputTypes[0] == Types::Vector3D &&
        InputTypes[1] == Types::Byte &&
        InputTypes[2] == Types::Byte)
        Temp.Write<Vector2D>(Vector2D(((Vector3D *)Inputs[0])->GetByIndex(*(uint8_t *)Inputs[1]), ((Vector3D *)Inputs[0])->GetByIndex(*(uint8_t *)Inputs[2])), OperationNumber(Start));

    return true;
}

bool Operation::Combine(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    void *Inputs[InputNumber];
    GetInputs(Start, InputTypes, Inputs, InputNumber);

    if (InputTypes[0] == Types::Vector2D &&
        InputTypes[1] == Types::Number)
        Temp.Write<Coord2D>(Coord2D(*(Vector2D *)Inputs[0], Vector2D(*(Number *)Inputs[1])), OperationNumber(Start));

    return true;
}

bool Operation::Add(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    void *Inputs[InputNumber];
    GetInputs(Start, InputTypes, Inputs, InputNumber);

    if (InputTypes[0] != InputTypes[1])
        return true;

    if (InputTypes[0] == Types::Vector2D)
        Temp.Write<Vector2D>(*(Vector2D *)Inputs[0] + *(Vector2D *)Inputs[1], OperationNumber(Start));

    return true;
}

bool Operation::Multiply(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    void *Inputs[InputNumber];
    GetInputs(Start, InputTypes, Inputs, InputNumber);

    if (InputTypes[0] != InputTypes[1])
        return true;

    if (InputTypes[0] == Types::Vector2D)
        Temp.Write<Vector2D>((*(Vector2D *)Inputs[0]) * (*(Vector2D *)Inputs[1]), OperationNumber(Start));
    if (InputTypes[0] == Types::Number)
        Temp.Write<Number>((*(Number *)Inputs[0]) * (*(Number *)Inputs[1]), OperationNumber(Start));

    return true;
}

bool Operation::MoveTo(int32_t Start)
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
            Modules.ValueSet<Coord2D>(((Coord2D *)Value)->TimeMove(*(Coord2D *)Target, *Time), Index);
        else if (Values.TypeAt(1) == Types::Number)
            Modules.ValueSet<Number>(TimeMove(*(Number *)Value, *(Number *)Target, *Time), Index);
    }

    return (CurrentTime >= *Time);
}

bool Operation::Delay(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    void *Inputs[InputNumber];
    GetInputs(Start, InputTypes, Inputs, InputNumber);

    if (InputTypes[0] != Types::Time || InputTypes[1] != Types::Time) // Values not avaliable
        return true;

    // 0 = delay, 1 = timer

    if (*(uint32_t *)Inputs[1] == 0)
        *(uint32_t *)Inputs[1] = CurrentTime;
    else if (CurrentTime > *(uint32_t *)Inputs[1] + *(uint32_t *)Inputs[0])
    {
        *(uint32_t *)Inputs[1] = 0; // Reset
        return true;                // Finished
    }
    return false;
}

bool Operation::AddDelay(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    void *Inputs[InputNumber];
    GetInputs(Start, InputTypes, Inputs, InputNumber);

    if (InputTypes[0] != Types::Time)
        return true;

    Temp.Write<uint32_t>(*(uint32_t *)Inputs[0] + CurrentTime, OperationNumber(Start));
    return true;
}

bool Operation::SetFlags(int32_t Start)
{
    FlagClass *Value = Values.At<FlagClass>(1);

    if (Value == nullptr)
        return true;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        Modules.At(Index)->Flags += Value->Values;

    return true;
}

bool Operation::ResetFlags(int32_t Start)
{
    FlagClass *Value = Values.At<FlagClass>(1);

    if (Value == nullptr)
        return true;

    for (int32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
        Modules.At(Index)->Flags -= Value->Values;

    return true;
}

bool Operation::Sine(int32_t Start)
{
    int32_t InputNumber = ParameterNumber(Start);
    Types InputTypes[InputNumber];
    void *Inputs[InputNumber];
    GetInputs(Start, InputTypes, Inputs, InputNumber);

    // 0 = X, 1 = Multiplier, 2 = Phase

    if (InputTypes[0] != Types::Time || InputTypes[1] != Types::Number || InputTypes[2] != Types::Number)
        return true;

    Temp.Write<Number>(sin((*(uint32_t *)Inputs[0]) * (*(Number *)Inputs[1]) + (*(Number *)Inputs[2])), OperationNumber(Start));

    return true;
}