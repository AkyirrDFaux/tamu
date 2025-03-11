Message *ReadValue(const Message &Input)
{
    if (Input.Type(1) != Types::ID || !Objects.IsValid(Input.Data<IDClass>(1)))
        return new Message(Status::InvalidID, Input);

    BaseClass *Object = Objects[Input.Data<IDClass>(1)];

    ByteArray Data = Object->GetValue();

    if (Data.Length == 0)
        return new Message(Status::NoValue, Input);

    return new Message(Functions::ReadValue, Input.Data<IDClass>(1), Data);
}

Message *WriteValue(const Message &Input)
{
    if (Input.Type(1) != Types::ID || !Objects.IsValid(Input.Data<IDClass>(1)))
        return new Message(Status::InvalidID, Input);

    BaseClass *Object = Objects[Input.Data<IDClass>(1)];

    if (Object->Flags == Flags::Auto)
        return new Message(Status::AutoObject, Input);
    else if (Input.Type(2) != Object->Type || Object->SetValue(Input.Segments[2]) == false)
        return new Message(Status::InvalidType, Input);

    ByteArray Data = Object->GetValue();
    return new Message(Functions::WriteValue, Input.Data<IDClass>(1), Data);
}

Message *ReadFile(const Message &Input)
{
    if (Input.Type(1) != Types::ID)
        return new Message(Status::InvalidID, Input);

    ByteArray Data = ReadFromFile(String(Input.Data<IDClass>(1)));
    return new Message(Functions::ReadFile, Input.Data<IDClass>(1), Data);
}