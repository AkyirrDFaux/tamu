void Run(const FlexArray &Input)
{
    if (Input.Length < 1)
    {
        char Response[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Response, sizeof(Response)) += Input);
        return;
    }

    Functions Function = (Functions)Input.Array[0];

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
    case Functions::Format:
        Format(Input);
        break;

    default:
        char Response[2] = {(char)Functions::Report, (char)Status::InvalidFunction};
        Chirp.Send(FlexArray(Response, sizeof(Response)) += Input);
        break;
    }
}

void CreateObject(const FlexArray &Input)
{
    // 1. Validation: Need at least Function(1) + Ref(3) + ObjectType(1) = 5 bytes
    if (Input.Length < 5)
    {
        char Response[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Response, 2) += Input);
        return;
    }

    // 2. Extract ID (Offset 1)
    // We cast the pointer to get the ID.
    Reference ID = Reference::Global(Input.Array[1], Input.Array[2], Input.Array[3]);
    ObjectTypes Type = (ObjectTypes)Input.Array[4];

    // 4. Check Registry for Duplicate ID
    if (Objects.At(ID) != nullptr)
    {
        char Response[2] = {(char)Functions::Report, (char)Status::InvalidID};
        Chirp.Send(FlexArray(Response, 2) += Input);
        return;
    }

    // 5. Create the Object
    BaseClass *Object = CreateObject(ID, Type);

    if (Object == nullptr)
    {
        char Response[2] = {(char)Functions::Report, (char)Status::NoValue};
        Chirp.Send(FlexArray(Response, 2) += Input);
    }
    else
    {
        // 6. Success Response: [Function][Type][ID...]
        char SuccessHeader[2] = {(char)Functions::CreateObject, (char)Object->Type};
        FlexArray Response(SuccessHeader, 2);

        // Append the variable-length ID
        Response += FlexArray((char *)ID.Data, 3);

        Chirp.Send(Response);
    }
}

void DeleteObject(const FlexArray &Input)
{
    // 1. Validation: We need 1 byte for Function + 3 bytes for Reference
    if (Input.Length < 4)
    {
        char Response[2] = {(char)Functions::Report, (char)Status::InvalidType};
        // Echo back the short/broken packet for debugging
        Chirp.Send(FlexArray(Response, 2) += Input);
        return;
    }

    // 2. Map the Reference directly from the buffer offset
    // This is O(1) - essentially free CPU-wise
    Reference ID = Reference::Global(Input.Array[1], Input.Array[2], Input.Array[3]);

    // 3. Resolve from the registry
    // Assuming your Registry 'Objects.At' is updated to take the 3-byte Reference
    BaseClass *Object = Objects.At(ID);

    // 4. Verify existence
    if (Object == nullptr)
    {
        char Response[2] = {(char)Functions::Report, (char)Status::InvalidID};
        Chirp.Send(FlexArray(Response, 2) += Input);
        return;
    }

    // 5. Execute hardware/registry cleanup
    Object->Destroy();

    // 6. Notify Success
    // Send back [DeleteObjectCode][Net][Group][Device]
    char SuccessHeader[1] = {(char)Functions::DeleteObject};
    FlexArray Success(SuccessHeader, 1);
    Success += FlexArray((char *)ID.Data, 3);

    Chirp.Send(Success);
}

void LoadObject(const FlexArray &Input)
{
    // Placeholder logic for app-side loading
    // Status err = Status::InvalidFunction;
    // Chirp.Send(ValueTree(&err, sizeof(Status), Types::Status));
}

void SaveObject(const FlexArray &Input)
{
    // 1. Validation: Need 1 byte for Function + 3 bytes for Reference
    if (Input.Length < 4)
    {
        char Response[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Response, 2) += Input);
        return;
    }

    // 2. Map the Reference directly (O(1) access)
    Reference ID = Reference::Global(Input.Array[1], Input.Array[2], Input.Array[3]);

    if (HW::Save(ID))
    {
        // 6. Notify Success: [SaveObjectCode][ReferenceData...]
        char SuccessHeader[1] = {(char)Functions::SaveObject};
        FlexArray Success(SuccessHeader, 1);

        // Append the variable-length ID as confirmation
        Success += FlexArray((char *)ID.Data, 3);

        Chirp.Send(Success);
    }
}

void SaveAll(const FlexArray &Input)
{
    // 2. Iterate through the Registry
    for (uint32_t Index = 0; Index < Objects.Registered; Index++)
    {
        RegisterEntry Obj = Objects.Object[Index];
        if (Obj.Object == nullptr)
            continue;

        if (Obj.Object->Flags == Flags::Dirty)
            HW::Save(Reference::Global(0, Obj.GroupID, Obj.DeviceID)); //Invalidates interanally
            
    }

    char SuccessHeader[1] = {(char)Functions::SaveAll};
    FlexArray Success(SuccessHeader, 1);
    Chirp.Send(Success);
}

void Format(const FlexArray &Input)
{
    HW::FlashFormat();

    // 2. Iterate through the Registry
    for (uint32_t Index = 0; Index < Objects.Registered; Index++)
    {
        RegisterEntry Obj = Objects.Object[Index];
        if (Obj.Object == nullptr)
            continue;

        Obj.Object->Flags += Flags::Dirty;
    }

    char SuccessHeader[1] = {(char)Functions::Format};
    FlexArray Success(SuccessHeader, 1);
    Chirp.Send(Success);
}

void ReadObject(const FlexArray &Input)
{
    // 1. Validation: Need 1 byte for Function + 3 bytes for Reference
    if (Input.Length < 4)
    {
        char Response[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Response, 2) += Input);
        return;
    }

    // 2. Map the Reference directly (Offset 1)
    Reference ID = Reference::Global(Input.Array[1], Input.Array[2], Input.Array[3]);

    // 3. Resolve from the registry
    BaseClass *Object = Objects.At(ID);

    // 4. Verify the object exists
    if (Object == nullptr)
    {
        char Response[2] = {(char)Functions::Report, (char)Status::InvalidID};
        Chirp.Send(FlexArray(Response, 2) += Input);
        return;
    }

    // 5. Build Success Response: [ReadObjectCode][CompressedData...]
    char SuccessHeader[1] = {(char)Functions::ReadObject};
    FlexArray Response(SuccessHeader, 1);

    // Concatenate the object's serialized state
    // This uses the Move Constructor if Compress() returns by value!
    Response += Object->Compress(false);

    Chirp.Send(Response);
}

void Refresh(const FlexArray &Input)
{
    // 1. Prepare the standard header once
    char funcHeader[1] = {(char)Functions::ReadObject};

    // 2. Iterate through the Registry
    for (uint32_t Index = 0; Index < Objects.Registered; Index++)
    {
        BaseClass *Obj = Objects.Object[Index].Object;
        if (Obj == nullptr)
            continue;

        // 3. Create a clean message for this specific object
        // [ReadObjectCode][CompressedData...]
        FlexArray msg(funcHeader, 1);
        msg += Obj->Compress(false); // Efficiently appends the object's data

        Chirp.Send(msg);
    }

    // 4. Signal completion of the sync burst
    char statusHeader[2] = {(char)Functions::Report, (char)Status::OK};
    Chirp.Send(FlexArray(statusHeader, 2));
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
    // 1. Create a 2-byte stack buffer: [ReportCode][StatusCode]
    char Response[2] = {(char)Functions::Report, (char)ErrorCode};

    // 2. Wrap in a FlexArray and send
    // This uses the (char*, size) constructor to copy the 2 bytes and ship them.
    Chirp.Send(FlexArray(Response, 2));
}