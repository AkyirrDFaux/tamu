void Run(const ByteArray &Input)
{
    SearchResult funcRes = Input.Find({0});

    if (funcRes.Length == 0 || funcRes.Type != Types::Function)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Functions Function = *(Functions *)funcRes.Value;

    switch (Function)
    {
    case Functions::CreateObject:
        CreateObject(Input);
        break;
    case Functions::DeleteObject:
        DeleteObject(Input);
        break;
    case Functions::LoadObject:
        LoadObject(Input);
        break;
    case Functions::SaveObject:
        SaveObject(Input);
        break;
    case Functions::SaveAll:
        SaveAll(Input);
        break;
    case Functions::ReadObject:
        ReadObject(Input);
        break;
    case Functions::Refresh:
        Refresh(Input);
        break;
    case Functions::ReadValue:
        ReadValue(Input);
        break;
    case Functions::WriteValue:
        WriteValue(Input);
        break;
    case Functions::ReadName:
        ReadName(Input);
        break;
    case Functions::WriteName:
        WriteName(Input);
        break;
    case Functions::ReadInfo:
        ReadInfo(Input);
        break;
    case Functions::SetInfo:
        SetInfo(Input);
        break;

    default:
        Status err = Status::InvalidFunction;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        break;
    }
}

void CreateObject(const ByteArray &Input)
{
    SearchResult idRes   = Input.Find({1}, true);
    SearchResult typeRes = Input.Find({2});

    if (idRes.Length == 0 || typeRes.Length == 0 || 
        idRes.Type != Types::Reference || typeRes.Type != Types::ObjectType)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Reference ID = *(Reference*)idRes.Value;
    ObjectTypes Type = *(ObjectTypes*)typeRes.Value;

    if (Objects.At(ID) != nullptr)
    {
        Status err = Status::InvalidID;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    BaseClass *Object = CreateObject(ID, Type);

    if (Object == nullptr)
    {
        Status err = Status::NoValue;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
    }
    else
    {
        // Define local variables for the response
        Functions funcCode = Functions::CreateObject;
        ObjectTypes objType = Object->Type;

        ByteArray Response(&funcCode, sizeof(Functions), Types::Function);
        Response << ByteArray(&objType, sizeof(ObjectTypes), Types::ObjectType);
        Response << ByteArray(&ID, sizeof(Reference), Types::Reference);
        
        Chirp.Send(Response);
    }
}

void DeleteObject(const ByteArray &Input)
{
    // 1. Resolve Reference {1}
    SearchResult idRes = Input.Find({1}, true);

    // 2. Validate existence and type
    if (idRes.Length == 0 || idRes.Type != Types::Reference)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Reference ID = *(Reference*)idRes.Value;

    // 3. Resolve the pointer from the registry
    BaseClass *Object = Objects.At(ID);

    // 4. Verify the object exists
    if (Object == nullptr)
    {
        Status err = Status::InvalidID;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    // 5. Check for Protected flags (if uncommented in the future)
    /* if (Object->Flags == Flags::Auto) { ... } */

    // 6. Execute destruction
    // This handles hardware disconnection and registry removal
    Object->Destroy();

    // 7. Notify Success
    Functions funcCode = Functions::DeleteObject;
    Chirp.Send(ByteArray(&funcCode, sizeof(Functions), Types::Function) << ByteArray(&ID, sizeof(Reference), Types::Reference));
}

void LoadObject(const ByteArray &Input)
{
    // Placeholder logic for app-side loading
    Status err = Status::InvalidFunction;
    Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status));
}

void SaveObject(const ByteArray &Input)
{
    // 1. Resolve Reference {1}
    SearchResult idRes = Input.Find({1}, true);

    // 2. Validate type
    if (idRes.Length == 0 || idRes.Type != Types::Reference)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Reference ID = *(Reference*)idRes.Value;

    // 3. Resolve the pointer
    BaseClass *Object = Objects.At(ID);

    // 4. Verify the object exists
    if (Object == nullptr)
    {
        Status err = Status::InvalidID;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    // 5. Execute Save (Serializes to Flash/EEPROM)
    Object->Save();

    // 6. Notify Success
    Functions funcCode = Functions::SaveObject;
    Chirp.Send(ByteArray(&funcCode, sizeof(Functions), Types::Function) << ByteArray(&ID, sizeof(Reference), Types::Reference));
}

void SaveAll(const ByteArray &Input)
{
    // TODO, First implement one-by-one correctly
    /*    
    HW::FlashFormat();
    MemoryReset();
    for (int32_t Index = 1; Index < Objects.Allocated; Index++)
    {
        if (Objects[IDClass(Index)] == nullptr || Objects[IDClass(Index)]->Flags == Flags::Auto)
            continue;
        Objects[IDClass(Index)]->Save();
    }
    Chirp.Send(ByteArray(Functions::SaveAll));*/
    return;
}
void ReadObject(const ByteArray &Input)
{
    // 1. Resolve Reference {1}
    SearchResult idRes = Input.Find({1}, true);

    // 2. Validate existence and type
    if (idRes.Length == 0 || idRes.Type != Types::Reference)
    {
        Status err = Status::InvalidType;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    Reference ID = *(Reference*)idRes.Value;

    // 3. Resolve the object pointer from the registry
    BaseClass *Object = Objects.At(ID);

    // 4. Validate that the object actually exists
    if (Object == nullptr)
    {
        Status err = Status::InvalidID;
        Chirp.Send(ByteArray(&err, sizeof(Status), Types::Status) << Input);
        return;
    }

    // 5. Serialize and Send the object data
    // We create a success header using local variables
    Functions funcCode = Functions::ReadObject;
    Chirp.Send(ByteArray(&funcCode, sizeof(Functions), Types::Function) << Object->Compress());
}

void Refresh(const ByteArray &Input)
{
    // Local variable for the repeated header
    Functions funcCode = Functions::ReadObject;

    // Use the Registry's internal count to iterate all objects
    for (uint32_t Index = 0; Index < Objects.Registered; Index++)
    {
        // Access the internal registry structure
        BaseClass *Obj = Objects.Object[Index].Object;
        if (Obj == nullptr)
            continue;

        // Push each object individually as a ReadObject message
        Chirp.Send(ByteArray(&funcCode, sizeof(Functions), Types::Function) << Obj->Compress());
    }

    // Signal completion of the sync burst
    Status ok = Status::OK;
    Chirp.Send(ByteArray(&ok, sizeof(Status), Types::Status));
}

/*

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
        Object->Name = Input.Get<Text>(4);
    if (ModulesType == Types::IDList)
        Object->Modules = Input.Get<IDList>(5);
    Object->InputValues(Input, 6);

    Chirp.Send(ByteArray(Functions::LoadObject) << ByteArray(*Object));
}


*/
void ReportError(Status ErrorCode)
{
    // Anchor the enum in a variable to provide a memory address
    Status err = ErrorCode;
    ByteArray Report(&err, sizeof(Status), Types::Status);
    
    Chirp.Send(Report);
}

void ReportError(Status ErrorCode, Reference ID)
{
    // Anchor both the status and the reference
    Status err = ErrorCode;
    Reference ref = ID;

    ByteArray Report(&err, sizeof(Status), Types::Status);
    Report << ByteArray(&ref, sizeof(Reference), Types::Reference);
    
    Chirp.Send(Report);
}