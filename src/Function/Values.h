void ReadValue(ByteArray &Input)
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
    ByteArray Data = Objects[ID]->OutputValues();

    if (Data.Length == 0)
    {
        Chirp.Send(ByteArray(Status::NoValue) << Input);
        return;
    }

    Chirp.Send(ByteArray(Functions::ReadValue) << ID << Data);
}

void WriteValue(ByteArray &Input)
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

    BaseClass *Object = Objects[ID];

    if (Object->Flags == Flags::Auto)
    {
        Chirp.Send(ByteArray(Status::AutoObject) << Input);
        return;
    }
    else if (Object->InputValues(Input, 2, ID.Sub()) == false)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }
    Chirp.Send(ByteArray(Functions::WriteValue) << ID << Object->OutputValues(ID.Sub()));
}

void ReadFile(ByteArray &Input)
{
    Types IDType = Input.Type(1);

    if (IDType != Types::ID)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    Chirp.Send(ByteArray(Functions::ReadFile) << HW::FlashLoad(Input.Get<IDClass>(1)));
}
