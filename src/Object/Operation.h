namespace Operation
{
    bool Equal(ByteArray &Values, Reference Index)
    {
        Reference Input0 = Index.Append(1).Append(0);
        Reference Output = Index.Append(0);

        return true;
    }

    bool Combine(ByteArray &Values, Reference Index)
    {
        Reference Input0 = Index.Append(1).Append(0);
        Reference Input1 = Index.Append(1).Append(1);
        Reference Output = Index.Append(0);

        Types Type0 = Values.Type(Input0);
        Types Type1 = Values.Type(Input1);

        if (Type0 == Types::Vector2D && Type1 == Types::Number)
        {
            Vector2D A = Values.Get<Vector2D>(Input0).Value;
            Number B = Values.Get<Number>(Input1).Value;
            
            // Combines a position (Vector2D) and rotation (Number) into Coord2D
            Values.Set(Coord2D(A, B), Output);
        }

        return true;
    }

    bool Add(ByteArray &Values, Reference Index)
    {
        Reference Input0 = Index.Append(1).Append(0);
        Reference Input1 = Index.Append(1).Append(1);
        Reference Output = Index.Append(0);

        Types Type0 = Values.Type(Input0);
        Types Type1 = Values.Type(Input1);

        if (Type0 == Types::Number && Type1 == Types::Number)
            Values.Set(Values.Get<Number>(Input0).Value + Values.Get<Number>(Input1).Value, Output);
        else if (Type0 == Types::Vector2D && Type1 == Types::Vector2D)
            Values.Set(Values.Get<Vector2D>(Input0).Value + Values.Get<Vector2D>(Input1).Value, Output);
        return true;
    }

    bool Multiply(ByteArray &Values, Reference Index)
    {
        Reference Input0 = Index.Append(1).Append(0);
        Reference Input1 = Index.Append(1).Append(1);
        Reference Output = Index.Append(0);

        Types Type0 = Values.Type(Input0);
        Types Type1 = Values.Type(Input1);

        if (Type0 == Types::Number && Type1 == Types::Number)
            Values.Set(Values.Get<Number>(Input0).Value * Values.Get<Number>(Input1).Value, Output);
        else if (Type0 == Types::Vector2D && Type1 == Types::Vector2D)
            Values.Set(Values.Get<Vector2D>(Input0).Value * Values.Get<Vector2D>(Input1).Value, Output);
        
        return true;
    }

    bool MoveTo(ByteArray &Values, Reference Index)
    {
        /*int32_t InputNumber = ParameterNumber(Start);
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
            for (uint32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
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
            for (uint32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
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
            for (uint32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
            {
                if (Modules.ValueTypeAt(Index) != Types::Number)
                    continue;
                Value = Modules.ValueGet<Number>(Index);
                Modules.ValueSet<Number>(TimeMove(Value, Target, Time), Index);
            }
        }
        else
            return true;
        return (CurrentTime >= Time);*/
        return true;
    }

    bool Delay(ByteArray &Values, Reference Index)
    {
        Reference InputDelay = Index.Append(1).Append(0);
        Reference StateTimer = Index.Append(2).Append(0); // Persistent state

        if (Values.Type(InputDelay) != Types::Integer) return true;

        int32_t Timer = Values.Get<int32_t>(StateTimer).Value;

        if (Timer == 0)
        {
            Values.Set(CurrentTime, StateTimer);
            return false;
        }
        else if (CurrentTime > Timer + Values.Get<int32_t>(InputDelay).Value)
        {
            Values.Set((int32_t)0, StateTimer); // Reset for next use
            return true; 
        }

        return false;
    }

    bool AddDelay(ByteArray &Values, Reference Index)
    {
        Reference Input0 = Index.Append(1).Append(0);
        Reference Output = Index.Append(0);

        if (Values.Type(Input0) != Types::Integer) return true;

        Values.Set(Values.Get<int32_t>(Input0).Value + CurrentTime, Output);
        return true;
    }

    bool SetFlags(ByteArray &Values, Reference Index)
    {
        /*if (Values.Type(1) != Types::Flags)
            return true;

        FlagClass Value = ValueGet<FlagClass>(1);

        for (uint32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
            Modules.At(Index)->Flags += Value.Values;
        */
        return true;
    }

    bool ResetFlags(ByteArray &Values, Reference Index)
    {
        /*if (Values.Type(1) != Types::Flags)
            return true;

        FlagClass Value = ValueGet<FlagClass>(1);

        for (uint32_t Index = 0; Index < Modules.Length; Modules.Iterate(&Index))
            Modules.At(Index)->Flags -= Value.Values;
        */
        return true;
    }

    bool Sine(ByteArray &Values, Reference Index)
    {
        Reference InTime  = Index.Append(1).Append(0);
        Reference InMult  = Index.Append(1).Append(1);
        Reference InPhase = Index.Append(1).Append(2);
        Reference Output  = Index.Append(0);

        if (Values.Type(InTime) != Types::Integer) return true;

        int32_t T = Values.Get<int32_t>(InTime).Value;
        Number M = Values.Get<Number>(InMult).Value;
        Number P = Values.Get<Number>(InPhase).Value;

        Values.Set(sin(T * M + P), Output);
        return true;
    }
}

bool RunOperation(Operations Operation, ByteArray &Values, Reference Index)
{
    switch (Operation)
    {
    case Operations::Equal:
        return Operation::Equal(Values, Index);
    case Operations::Combine:
        return Operation::Combine(Values, Index);
    case Operations::Add:
        return Operation::Add(Values, Index);
    case Operations::Multiply:
        return Operation::Multiply(Values, Index);
    case Operations::MoveTo:
        return Operation::MoveTo(Values, Index);
    case Operations::Delay:
        return Operation::Delay(Values, Index);
    case Operations::AddDelay:
        return Operation::AddDelay(Values, Index);
    case Operations::SetFlags:
        return Operation::SetFlags(Values, Index);
    case Operations::ResetFlags:
        return Operation::ResetFlags(Values, Index);
    case Operations::Sine:
        return Operation::Sine(Values, Index);
    default:
        ReportError(Status::InvalidValue);
        return true;
    }
    return true;
}