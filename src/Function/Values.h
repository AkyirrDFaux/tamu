void ReadValue(const ByteArray &Input)
{
    SearchResult idRes = Input.Find({1}, true);
    if (idRes.Length == 0 || idRes.Type != Types::Reference)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Reference ID = *(Reference *)idRes.Value;
    BaseClass *Object = Objects.At(ID);

    if (!Object)
    {
        Status err = Status::InvalidID;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    ByteArray Data;
    // If Path exists in the Reference, copy just that sub-node.
    if (ID.PathLen() > 0)
        Data = Object->Values.Copy(ID);
    else
        Data = Object->Values; // Otherwise copy the whole tree

    if (Data.Length == 0)
    {
        Status err = Status::NoValue;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Functions func = Functions::ReadValue;
    Chirp.Send(ByteArray(&func, sizeof(Functions), Types::Function) << ByteArray(&ID, sizeof(Reference), Types::Reference) << Data);
}

void WriteValue(const ByteArray &Input)
{
    SearchResult idRes = Input.Find({1}, true);
    if (idRes.Length == 0 || idRes.Type != Types::Reference)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Reference ID = *(Reference *)idRes.Value;
    BaseClass *Object = Objects.At(ID);
    if (Object == nullptr)
    {
        Status err = Status::InvalidID;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    // Locate the payload at index {2}
    SearchResult payload = Input.Find({2}, true);
    if (payload.Length == 0)
    {
        Status err = Status::NoValue;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    // Update the Object's internal Values at the path specified by the Reference
    Object->ValueSetup(payload.Value, payload.Length, payload.Type, ID);

    // Confirmation Echo: Function + Reference + Current State
    Functions func = Functions::ReadValue;
    Chirp.Send(ByteArray(&func, sizeof(Functions), Types::Function) << ByteArray(&ID, sizeof(Reference), Types::Reference) << Object->Values.Copy(ID));
}

void ReadName(const ByteArray &Input)
{
    SearchResult idRes = Input.Find({1}, true);
    if (idRes.Length == 0 || idRes.Type != Types::Reference)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Reference ID = *(Reference *)idRes.Value;
    BaseClass *Object = Objects.At(ID);

    if (Object == nullptr)
    {
        Status err = Status::InvalidID;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Functions func = Functions::ReadName;
    Chirp.Send(ByteArray(&func, sizeof(Functions), Types::Function) << ByteArray(&ID, sizeof(Reference), Types::Reference) << ByteArray(Object->Name.Data, Object->Name.Length, Types::Text));
}

void WriteName(const ByteArray &Input)
{
    SearchResult idRes = Input.Find({1}, true);
    SearchResult nameRes = Input.Find({2}, true);

    if (idRes.Length == 0 || nameRes.Length == 0 || idRes.Type != Types::Reference)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Reference ID = *(Reference *)idRes.Value;
    BaseClass *Object = Objects.At(ID);

    if (Object == nullptr)
    {
        Status err = Status::InvalidID;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    // Direct update to the Text object (assuming internal buffer management)
    Object->Name = *(Text *)nameRes.Value;

    Functions func = Functions::ReadName;
    Chirp.Send(ByteArray(&func, sizeof(Functions), Types::Function) << ByteArray(&ID, sizeof(Reference), Types::Reference) << ByteArray(Object->Name.Data, Object->Name.Length, Types::Text));
}

void ReadInfo(const ByteArray &Input)
{
    SearchResult idRes = Input.Find({1}, true);
    if (idRes.Length == 0 || idRes.Type != Types::Reference)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Reference ID = *(Reference *)idRes.Value;
    int32_t Index = Objects.Search(ID);

    if (Index == -1)
    {
        Status err = Status::InvalidID;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Functions func = Functions::ReadInfo;
    Chirp.Send(ByteArray(&func, sizeof(Functions), Types::Function) << ByteArray(&ID, sizeof(Reference), Types::Reference) << ByteArray(&Objects.Object[Index].Object->Flags, 3, Types::ObjectInfo));
}

void SetInfo(const ByteArray &Input)
{
    SearchResult idRes = Input.Find({1}, true);
    SearchResult infoRes = Input.Find({2}, true);

    if (idRes.Length == 0 || infoRes.Length == 0 || idRes.Type != Types::Reference)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Reference ID = *(Reference *)idRes.Value;
    int32_t Index = Objects.Search(ID);

    if (Index == -1)
    {
        Status err = Status::InvalidID;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    // Update the internal registry info
    Objects.Object[Index].Object->Flags = ((FlagClass *)infoRes.Value)[0];
    Objects.Object[Index].Object->RunPeriod = ((uint8_t *)infoRes.Value)[1];
    Objects.Object[Index].Object->RunPhase = ((uint8_t *)infoRes.Value)[2];
    //Update the runner registry
    Objects.Object[Index].Info = {Objects.Object[Index].Object->RunPeriod, Objects.Object[Index].Object->RunPhase};

    Functions func = Functions::ReadInfo;
    Chirp.Send(ByteArray(&func, sizeof(Functions), Types::Function) << ByteArray(&ID, sizeof(Reference), Types::Reference) << ByteArray(&Objects.Object[Index].Object->Flags, 3, Types::ObjectInfo));
}