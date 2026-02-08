void Run(ByteArray &Input)
{
    if (Input.Type(0) != Types::Function)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    Functions Function = Input.Get<Functions>(0);

    if (Function == Functions::ReadDatabase)
        ReadDatabase(Input);
    else if (Function == Functions::CreateObject)
        CreateObject(Input);
    else if (Function == Functions::DeleteObject)
        DeleteObject(Input);
    else if (Function == Functions::WriteValue)
        WriteValue(Input);
    else if (Function == Functions::ReadValue)
        ReadValue(Input);
    else if (Function == Functions::WriteName)
        WriteName(Input);
    else if (Function == Functions::ReadName)
        ReadName(Input);
    else if (Function == Functions::SaveObject)
        SaveObject(Input);
    // else if (Function == Functions::ReadFile)
    //     ReadFile(Input);
    else if (Function == Functions::SaveAll)
        SaveAll(Input);
    // else if (Function == Functions::RunFile)
    //     RunFile(Input);
    else if (Function == Functions::ReadObject)
        ReadObject(Input);
    else if (Function == Functions::LoadObject)
        LoadObject(Input);
    else if (Function == Functions::Refresh)
        Refresh(Input);
    else if (Function == Functions::SetModules)
        SetModules(Input);
    else if (Function == Functions::SetFlags)
        SetFlags(Input);
    else
        Chirp.Send(ByteArray(Status::InvalidFunction) << Input);
    return;
}

void ReadDatabase(ByteArray &Input)
{
    //Objects.ContentDebug();
}

void Refresh(ByteArray &Input)
{
    for (uint32_t Index = 1; Index <= Objects.Allocated; Index++)
    {
        if (!Objects.IsValid(IDClass(Index, 0)))
            continue;
        Chirp.Send(ByteArray(Functions::ReadObject) << ByteArray(*Objects[IDClass(Index, 0)]));
    }
    Chirp.Send(ByteArray(Status::OK));
}

void CreateObject(ByteArray &Input)
{
    BaseClass *Object = nullptr;
    Types TypeType = Input.Type(1);
    Types IDType = Input.Type(2);
    ObjectTypes Type;
    IDClass ID;
    if (TypeType == Types::ObjectType && IDType == Types::ID)
    {
        Type = Input.Get<ObjectTypes>(1);
        ID = Input.Get<IDClass>(2);
        if (!Objects.IsValid(ID)) // Check for ID colision
            Object = CreateObject(Type, true, ID);
        else
        {
            Chirp.Send(ByteArray(Status::InvalidID) << Input);
            return;
        }
    }
    else if (TypeType == Types::ObjectType)
    {
        Type = Input.Get<ObjectTypes>(1);
        Object = CreateObject((ObjectTypes)Type);
    }

    else
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    if (Object == nullptr)
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
    else
        Chirp.Send(ByteArray(Functions::CreateObject) << ByteArray(Object->Type) << ByteArray(Object->ID));
}

void LoadObject(ByteArray &Input)
{
    BaseClass *Object = nullptr;
    Types IDType = Input.Type(1);
    Types TypeType = Input.Type(2);

    if (IDType != Types::ID || TypeType != Types::ObjectType)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    IDClass ID = Input.Get<IDClass>(1);
    ObjectTypes Type = Input.Get<ObjectTypes>(2);

    if (!Objects.IsValid(ID)) // Check for ID colision
        Object = CreateObject(Type, false, ID);
    else if (Objects[ID]->Type == Type) // Overwrite the object, since the type and ID are same
        Object = Objects[ID];
    else
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    Types FlagsType = Input.Type(3);
    Types NameType = Input.Type(4);
    Types ModulesType = Input.Type(5);

    if (FlagsType == Types::Flags)
        Object->Flags = Input.Get<FlagClass>(3);
    if (NameType == Types::Text)
        Object->Name = Input.Get<String>(4);
    if (ModulesType == Types::IDList)
        Object->Modules = Input.Get<IDList>(5);
    Object->InputValues(Input, 6);

    Chirp.Send(ByteArray(Functions::LoadObject) << ByteArray(*Object));
}

void DeleteObject(ByteArray &Input)
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

    delete Objects[ID];
    Chirp.Send(ByteArray(Functions::DeleteObject) << ID);
}

void SetModules(ByteArray &Input)
{
    Types IDType = Input.Type(1);
    Types IDListType = Input.Type(2);

    if (IDType != Types::ID || IDListType != Types::IDList)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    IDClass ID = Input.Get<IDClass>(1);
    IDList Modules = Input.Get<IDList>(2);

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

    Objects[ID]->Modules = Modules;
    Chirp.Send(ByteArray(Functions::SetModules) << ID << Modules);
};

void SetFlags(ByteArray &Input)
{
    Types IDType = Input.Type(1);
    Types FlagsType = Input.Type(2);

    if (IDType != Types::ID || FlagsType != Types::Flags)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    IDClass ID = Input.Get<IDClass>(1);
    FlagClass Flags = Input.Get<FlagClass>(2);

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

    Objects[ID]->Flags = Flags;
    if (Objects[ID]->Type == ObjectTypes::Program && Objects[ID]->Flags == Flags::RunOnce)
        Objects[ID]->ValueSet<int32_t>(0, 1); // Reset counter
    Chirp.Send(ByteArray(Functions::SetFlags) << ID << Flags);
};

void WriteName(ByteArray &Input)
{
    Types IDType = Input.Type(1);
    Types NameType = Input.Type(2);

    if (IDType != Types::ID || NameType != Types::Text)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    IDClass ID = Input.Get<IDClass>(1);
    String Name = Input.Get<String>(2);

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

    Objects[ID]->Name = Name;
    Chirp.Send(ByteArray(Functions::WriteName) << ID << Name);
};

void ReadName(ByteArray &Input)
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

    Chirp.Send(ByteArray(Functions::ReadName) << ID << ByteArray(Objects[ID]->Name));
};

void ReportError(Status ErrorCode)
{
    ByteArray Report = ByteArray(ErrorCode);
    Chirp.SendNow(Report);
}