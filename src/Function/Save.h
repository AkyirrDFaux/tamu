void BaseClass::Save()
{
    ByteArray Data = ByteArray(Types::Function) << ByteArray(Functions::LoadObject);
    Data = Data << ByteArray(*this);
    Data.WriteToFile(String(ID.ID));
};

Message *SaveObject(const Message &Input)
{
    if (Input.Type(1) != Types::ID || !Objects.IsValid(Input.Data<IDClass>(1)))
        return new Message(Status::InvalidID, Input);
    if (Objects[Input.Data<IDClass>(1)]->Flags == Flags::Auto)
        return new Message(Status::AutoObject);
    else
        Objects[Input.Data<IDClass>(1)]->Save();

    return new Message(Functions::SaveObject, Input.Data<IDClass>(1));
};

Message *SaveAll(const Message &Input)
{
    MemoryReset();
    for (int32_t Index = 1; Index < Objects.Allocated; Index++)
    {
        if (Objects[IDClass(Index)] == nullptr || Objects[IDClass(Index)]->Flags == Flags::Auto)
            continue;
        Objects[IDClass(Index)]->Save();
    }
    return new Message(Functions::SaveAll);
}

Message *ReadObject(const Message &Input)
{
    if (Input.Type(1) != Types::ID || !Objects.IsValid(Input.Data<IDClass>(1)))
        return new Message(Status::InvalidID, Input);

    ByteArray Data = ByteArray(*Objects[Input.Data<IDClass>(1)]);
    return new Message(Functions::ReadObject, Data);
};

Message *RunFile(const Message &Input)
{
    if (Input.Type(1) != Types::ID)
        return new Message(Status::InvalidID, Input);

    ByteArray Data = ReadFromFile(String(Input.Data<IDClass>(1)));
    
    return Message(Data).Run();
};