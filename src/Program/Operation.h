class Operation : public Variable<Operations>
{
public:
    Operation(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~Operation() { };
    bool Run();
};

Operation::Operation(bool New, IDClass ID, FlagClass Flags) : Variable(Operations::None, ID, Flags)
{
    Type = Types::Operation;
    Name = "Operation";
};

bool Operation::Run()
{
    switch (*Data)
    {
    case Operations::Equal:
        if (Modules.IsValid(0) == false)
            return true;
        for (int32_t Index = Modules.FirstValid(Modules[0]->Type, 1); Index < Modules.Length; Modules.Iterate(&Index, Modules[0]->Type))
        {
            if (GetValueSize(Modules[0]->Type) == sizeof(uint8_t))
                *Modules.GetValue<uint8_t>(Index) = *Modules.GetValue<uint8_t>(0);
            else if (GetValueSize(Modules[0]->Type) == sizeof(uint32_t))
                *Modules.GetValue<uint32_t>(Index) = *Modules.GetValue<uint32_t>(0);
            else if (GetValueSize(Modules[0]->Type) == sizeof(Vector2D))
                *Modules.GetValue<Vector2D>(Index) = *Modules.GetValue<Vector2D>(0);
            else if (GetValueSize(Modules[0]->Type) == sizeof(Coord2D))
                *Modules.GetValue<Coord2D>(Index) = *Modules.GetValue<Coord2D>(0);
            else if (Modules[0]->Type == Types::Text)
                *Modules.GetValue<String>(Index) = *Modules.GetValue<String>(0);
        }
        return true;
    case Operations::IsEqual:
    case Operations::IsGreater:
    case Operations::IsSmaller:
        ReportError(Status::InvalidValue, "Operation not implemeted");
        return true;
    case Operations::Add:
        if (!Modules.IsValid(0) || !Modules.IsValid(1) || Modules[0]->Type != Modules[1]->Type)
            return true;
        for (int32_t Index = Modules.FirstValid(Modules[0]->Type, 2); Index < Modules.Length; Modules.Iterate(&Index, Modules[0]->Type))
        {
            if (Modules[0]->Type == Types::Number)
                *Modules.GetValue<float>(Index) = *Modules.GetValue<float>(0) + *Modules.GetValue<float>(1);
        }
        return true;
    case Operations::Subtract:
    case Operations::Multiply:
    case Operations::Divide:
    case Operations::Power:
    case Operations::Absolute:
    case Operations::Rotate:
    case Operations::RandomBetween:
        ReportError(Status::InvalidValue, "Operation not implemeted");
        return true;
    case Operations::AddDelay:
        if (Modules.IsValid(0) == false || Modules[0]->Type != Types::Time)
            return true;
        for (int32_t Index = Modules.FirstValid(Types::Time, 1); Index < Modules.Length; Modules.Iterate(&Index, Types::Time))
            *Modules.GetValue<uint32_t>(Index) = *Modules.GetValue<uint32_t>(0) + CurrentTime;
        return true;
    case Operations::IfSwitch:
    case Operations::While:
    case Operations::SetActivity:
        ReportError(Status::InvalidValue, "Operation not implemeted");
        return true;
    default:
        break;
    }
    return true;
}