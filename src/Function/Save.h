void BaseClass::Save()
{
    ByteArray Data = ByteArray(Functions::LoadObject) << ByteArray(*this);
    //Data.WriteToFile(String(ID.ID));
};

void SaveObject(ByteArray &Input)
{
    Types IDType = Input.Type(1);

    if (IDType != Types::ID)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    IDClass ID = Input.Get<IDClass>(1);

    if (!Objects.IsValid(ID))
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }
    else if (Objects[ID]->Flags == Flags::Auto)
    {
        Chirp.Send(ByteArray(Status::AutoObject) << Input);
        return;
    }

    Objects[ID]->Save();
    Chirp.Send(ByteArray(Functions::SaveObject) << ID);
};

void SaveAll(ByteArray &Input)
{
    MemoryReset();
    for (int32_t Index = 1; Index < Objects.Allocated; Index++)
    {
        if (Objects[IDClass(Index)] == nullptr || Objects[IDClass(Index)]->Flags == Flags::Auto)
            continue;
        Objects[IDClass(Index)]->Save();
    }
    Chirp.Send(ByteArray(Functions::SaveAll));
}

void ReadObject(ByteArray &Input)
{
    Types IDType = Input.Type(1);

    if (IDType != Types::ID)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    IDClass ID = Input.Get<IDClass>(1);

    if (!Objects.IsValid(ID))
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    Chirp.Send(ByteArray(Functions::ReadObject) << ByteArray(*Objects[ID]));
};

/*
void RunFile(ByteArray &Input)
{
    Types IDType = Input.Type(1);

    if (IDType != Types::ID)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    IDClass ID = Input.Get<IDClass>(1);

    if (!Objects.IsValid(ID))
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    ByteArray Data = ReadFromFile(String(ID.As<IDClass>()));
    Run(Data);
};
*/