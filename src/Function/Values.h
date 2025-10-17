void ReadValue(ByteArray &Input)
{
    ByteArray ID = Input.ExtractPart();
    if (ID.Type() != Types::ID || !Objects.IsValid(ID))
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }
    ByteArray Data = Objects[ID]->GetValue();

    if (Data.Length == 0)
    {
        Chirp.Send(ByteArray(Status::NoValue) << Input);
        return;
    }

    Chirp.Send(ByteArray(Functions::ReadValue) << ID << Data);
}

void WriteValue(ByteArray &Input)
{
    ByteArray ID = Input.ExtractPart();
    if (ID.Type() != Types::ID || !Objects.IsValid(ID))
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    BaseClass *Object = Objects[ID];

    if (Object->Flags == Flags::Auto)
    {
        Chirp.Send(ByteArray(Status::AutoObject) << Input);
        return;
    }
    else if (Object->SetValue(Input) == false)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }
    Chirp.Send(ByteArray(Functions::WriteValue) << ID << Object->GetValue());
}

void ReadFile(ByteArray &Input)
{
    ByteArray ID = Input.ExtractPart();
    if (ID.Type() != Types::ID || !Objects.IsValid(ID))
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    Chirp.Send(ByteArray(Functions::ReadFile) << ID << ReadFromFile(String(ID.As<IDClass>())));
}