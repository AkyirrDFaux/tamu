void Run(const ByteArray &Input)
{
    Getter<Functions> Function = Input.Get<Functions>({0});
    if (Function.Success == false)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    switch (Function.Value)
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
        // Handle unknown or unmapped functions
        Chirp.Send(ByteArray(Status::InvalidFunction) << Input);
        break;
    }
    return;
}

void CreateObject(const ByteArray &Input)
{
    // 1. Attempt to resolve the required parameters
    Getter<Reference> ID = Input.Get<Reference>({1});
    Getter<ObjectTypes> Type = Input.Get<ObjectTypes>({2});

    // 2. Validate existence and type alignment in one go
    if (!Type.Success || !ID.Success)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    // 3. Check for ID collision using the implicit conversion to Reference
    if (Objects.At(ID) != nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    // 4. Create the Actual Object using implicit conversions for both Type and ID
    BaseClass *Object = CreateObject(ID, Type);

    // 5. Finalize and Respond
    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::NoValue) << Input);
    }
    else
    {
        // Success: Echo back the Type and the Reference
        Chirp.Send(ByteArray(Functions::CreateObject) << ByteArray(Object->Type) << ByteArray(ID.Value));
    }
}

void DeleteObject(const ByteArray &Input)
{
    // 1. Attempt to resolve the Reference from the input
    Getter<Reference> ID = Input.Get<Reference>({1});

    // 2. Validate type alignment
    if (!ID.Success)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    // 3. Resolve the pointer from the registry
    BaseClass *Object = Objects.At(ID);

    // 4. Verify the object exists
    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    // 5. Check for Protected/Auto flags to prevent accidental deletion
    /*if (Object->Flags == Flags::Auto)
    {
        Chirp.Send(ByteArray(Status::AutoObject) << Input);
        return;
    }*/

    // 6. Execute high-level destruction
    // This triggers the virtual cleanup, Disconnects from the Board,
    // and finally removes itself from the Objects registry.
    Object->Destroy();

    // 7. Notify Success
    Chirp.Send(ByteArray(Functions::DeleteObject) << ByteArray(ID.Value));
}

void LoadObject(const ByteArray &Input)
{
    //TODO, load from app
    Chirp.Send(ByteArray(Status::InvalidFunction));
    return;
}

void SaveObject(const ByteArray &Input)
{
    // 1. Resolve the Reference using the optimized Getter
    Getter<Reference> ID = Input.Get<Reference>({1});

    // 2. Validate type alignment
    if (!ID.Success)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    // 3. Resolve the pointer from the registry
    BaseClass *Object = Objects.At(ID);

    // 4. Verify the object exists
    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    // 5. Execute the Save procedure
    // This triggers the object's internal serialization to non-volatile memory
    Object->Save();

    // 6. Notify Success
    // Using the ID getter's implicit conversion to Reference for the response
    Chirp.Send(ByteArray(Functions::SaveObject) << ByteArray(ID.Value));
}

void SaveAll(const ByteArray &Input)
{
    //TODO, First implement one-by-one correctly
    Chirp.Send(ByteArray(Status::InvalidFunction));
    return;
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
}
void ReadObject(const ByteArray &Input)
{
    // 1. Get the Reference using the optimized Getter
    Getter<Reference> ID = Input.Get<Reference>({1});

    // 2. Validate existence and type in one step
    if (!ID.Success)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    // 3. Resolve the object pointer from the registry
    BaseClass *Object = Objects.At(ID);

    // 4. Validate that the object actually exists
    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    // 5. Serialize and Send the object data
    // We dereference the pointer to pass the BaseClass instance
    // to the ByteArray builder.
    Chirp.Send(ByteArray(Functions::ReadObject) << ByteArray(*Object));
}

void Refresh(const ByteArray &Input)
{
    // Use the Registry's internal count
    for (uint32_t Index = 0; Index < Objects.Registered; Index++)
    {
        // Safety check: Ensure the registry slot isn't empty
        BaseClass* Obj = Objects.Object[Index].Object;
        if (Obj == nullptr) continue;

        // Serialize the actual object data
        Chirp.Send(ByteArray(Functions::ReadObject) << ByteArray(*Obj));
    }
    Chirp.Send(ByteArray(Status::OK));
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
    ByteArray Report = ByteArray(ErrorCode);
    Chirp.Send(Report);
}
void ReportError(Status ErrorCode, Reference ID)
{
    ByteArray Report = ByteArray(ErrorCode) << ByteArray(ID);
    Chirp.Send(Report);
}
