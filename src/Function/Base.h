Message *Message::Run()
{
    if (Type(0) == Types::Function)
    {
        Functions Function = Data<Functions>(0);
        if (Function == Functions::ReadDatabase)
            return ReadDatabase(*this);
        else if (Function == Functions::CreateObject)
            return CreateObject(*this);
        else if (Function == Functions::DeleteObject)
            return DeleteObject(*this);
        else if (Function == Functions::WriteValue)
            return WriteValue(*this);
        else if (Function == Functions::ReadValue)
            return ReadValue(*this);
        else if (Function == Functions::WriteName)
            return WriteName(*this);
        else if (Function == Functions::ReadName)
            return ReadName(*this);
        else if (Function == Functions::SaveObject)
            return SaveObject(*this);
        else if (Function == Functions::ReadFile)
            return ReadFile(*this);
        else if (Function == Functions::SaveAll)
            return SaveAll(*this);
        else if (Function == Functions::RunFile)
            return RunFile(*this);
        else if (Function == Functions::ReadObject)
            return ReadObject(*this);
        else if (Function == Functions::LoadObject)
            return LoadObject(*this);
        else if (Function == Functions::Refresh)
            return Refresh(*this);
        else if (Function == Functions::SetModules)
            return SetModules(*this);
        else if (Function == Functions::SetFlags)
            return SetFlags(*this);
        else
            return new Message(Status::InvalidFunction, this);
    }
    return new Message(Status::InvalidType, this);
}

Message *ReadDatabase(const Message &Input)
{
    Serial.println(Objects.ContentDebug());
    return new Message();
}

Message *Refresh(const Message &Input)
{
    for (uint32_t Index = 1; Index <= Objects.Allocated; Index++)
    {
        if (!Objects.IsValid(IDClass(Index)))
            continue;

        ByteArray Data = ByteArray(*Objects[IDClass(Index)]);
        Chirp.Send(Message(Functions::ReadObject, Data));
    }
    return new Message(Status::OK);
}

BaseClass *CreateObject(Types Type, bool New, IDClass ID, FlagClass Flags)
{
    switch (Type)
    {
    case Types::Folder:
        return new Folder(New, ID, Flags);
    // Port is auto
    case Types::Shape2D:
        return new Shape2DClass(New, ID, Flags);
    case Types::Byte:
        return new Variable<u_int8_t>(0, ID, Flags);
    case Types::Bool:
        return new Variable<bool>(0, ID, Flags);
    // Type is internal
    // Function is internal
    // Flags is internal
    // Status is internal
    case Types::Board:
        return new BoardClass(New, ID, Flags);
        // PortDriver is auto
    case Types::Fan:
        return new FanClass(New, ID, Flags);
    case Types::LEDSegment:
        return new LEDSegmentClass(New, ID, Flags);
    case Types::Texture1D:
        return new Texture1D(New, ID, Flags);
    case Types::LEDStrip:
        return new LEDStripClass(New, ID, Flags);
    case Types::Geometry2D:
        return new Geometry2DClass(New, ID, Flags);
    case Types::GeometryOperation:
        return new Variable<GeometryOperation>(GeometryOperation::Add, ID, Flags);
    case Types::Texture2D:
        return new Texture2D(New, ID, Flags);
    case Types::Display:
        return new DisplayClass(New, ID, Flags);
    case Types::AnimationFloat:
        return new AnimationFloat(New, ID, Flags);
    // case Types::AnimationVector
    case Types::AnimationCoord:
        return new AnimationCoord(New, ID, Flags);
    // case Types::AnimationColour
    case Types::Operation:
        return new Operation(New, ID, Flags);
    case Types::Program:
        return new Program(New, ID, Flags);
    case Types::Integer:
        return new Variable<int32_t>(0, ID, Flags);
    case Types::Time:
        return new Variable<u_int32_t>(0, ID, Flags);
    case Types::Number:
        return new Variable<float>(0.0F, ID, Flags);
    // ID is internal
    case Types::Colour:
        return new Variable<ColourClass>(ColourClass(), ID, Flags);
    case Types::PortAttach:
        return new PortAttachClass(New, ID, Flags);
    case Types::Vector2D:
        return new Variable<Vector2D>(Vector2D(), ID, Flags);
    case Types::Coord2D:
        return new Variable<Coord2D>(Coord2D(), ID, Flags);
    case Types::Text:
        return new Variable<String>("", ID, Flags);
    // IDList is internal
    default:
        ReportError(Status::InvalidType, "Type not allowed for BaseClass creation:" + String((uint8_t)Type));
        return nullptr;
    }
}

Message *CreateObject(const Message &Input)
{
    BaseClass *Object = nullptr;
    if (Input.Type(1) == Types::Type && Input.Type(2) == Types::ID)
    {
        if (!Objects.IsValid(Input.Data<IDClass>(2))) // Check for ID colision
            Object = CreateObject(Input.Data<Types>(1), true, Input.Data<IDClass>(2));
        else
            return new Message(Status::InvalidID);
    }
    else if (Input.Type(1) == Types::Type)
        Object = CreateObject(Input.Data<Types>(1));
    else
        return new Message(Status::InvalidType, Input);

    if (Object == nullptr)
        return new Message(Status::InvalidType, Input);
    return new Message(Functions::CreateObject, Object->Type, Object->ID);
}

Message *LoadObject(const Message &Input)
{
    BaseClass *Object = nullptr;
    if (Input.Type(1) == Types::Type && Input.Type(2) == Types::ID)
    {
        if (!Objects.IsValid(Input.Data<IDClass>(2))) // Check for ID colision
            Object = CreateObject(Input.Data<Types>(1), false, Input.Data<IDClass>(2));
        else if (Objects[Input.Data<IDClass>(2)]->Type == Input.Data<Types>(1)) // Overwrite the object, since the type and ID are same
            Object = Objects[Input.Data<IDClass>(2)];
        else
            return new Message(Status::InvalidID);
    }
    else
        return new Message(Status::InvalidType, Input);

    if (Object == nullptr)
        return new Message(Status::InvalidType, Input);

    if (Input.Type(3) == Types::Flags)
        Object->Flags = Input.Data<Flags>(3);
    if (Input.Type(4) == Types::Text)
        Object->Name = Input.Data<String>(4);
    if (Input.Type(5) == Types::IDList)
        Object->Modules = Input.Data<IDList>(5); // Fixed, careful
    if (Input.Type(6) == Object->Type)
        Object->SetValue(Input.Segments[6]);

    return new Message(Functions::LoadObject, ByteArray(*Object));
}

Message *DeleteObject(const Message &Input)
{
    if (Input.Type(1) != Types::ID || !Objects.IsValid(Input.Data<IDClass>(1)))
        return new Message(Status::InvalidID, Input);
    else if (Objects[Input.Data<IDClass>(1)]->Flags == Flags::Auto)
        return new Message(Status::AutoObject, Input);
    else
        delete Objects[Input.Data<IDClass>(1)];
    return new Message(Functions::DeleteObject, Input.Data<IDClass>(1));
}

Message *SetModules(const Message &Input)
{
    if (Input.Type(1) != Types::ID || !Objects.IsValid(Input.Data<IDClass>(1)))
        return new Message(Status::InvalidID, Input);
    else if (Objects[Input.Data<IDClass>(1)]->Flags == Flags::Auto)
        return new Message(Status::AutoObject, Input);
    else if (Input.Type(2) == Types::IDList)
        Objects[Input.Data<IDClass>(1)]->Modules = Input.Data<IDList>(2); // Fixed, careful
    else
        return new Message(Status::InvalidType, Input);
    return new Message(Functions::SetModules, Input.Data<IDClass>(1), Objects[Input.Data<IDClass>(1)]->Modules);
};

Message *SetFlags(const Message &Input)
{
    if (Input.Type(1) != Types::ID || !Objects.IsValid(Input.Data<IDClass>(1)))
        return new Message(Status::InvalidID, Input);
    else if (Objects[Input.Data<IDClass>(1)]->Flags == Flags::Auto)
        return new Message(Status::AutoObject, Input);
    else if (Input.Type(2) == Types::Flags)
        Objects[Input.Data<IDClass>(1)]->Flags = Input.Data<Flags>(2);
    else
        return new Message(Status::InvalidType, Input);
    return new Message(Functions::SetFlags, Input.Data<IDClass>(1), Objects[Input.Data<IDClass>(1)]->Flags);
};

Message *WriteName(const Message &Input)
{
    if (Input.Type(1) != Types::ID || !Objects.IsValid(Input.Data<IDClass>(1)))
        return new Message(Status::InvalidID, Input);
    else if (Objects[Input.Data<IDClass>(1)]->Flags == Flags::Auto)
        return new Message(Status::AutoObject, Input);
    else if (Input.Type(2) != Types::Text)
        return new Message(Status::InvalidType, Input);
    else
        Objects[Input.Data<IDClass>(1)]->Name = Input.Data<String>(2);
    return new Message(Functions::WriteName, Input.Data<IDClass>(1), Input.Data<String>(2));
};

Message *ReadName(const Message &Input)
{
    if (Input.Type(1) != Types::ID || !Objects.IsValid(Input.Data<IDClass>(1)))
        return new Message(Status::InvalidID, Input);

    return new Message(Functions::ReadName, Input.Data<IDClass>(1), Objects[Input.Data<IDClass>(1)]->Name);
};
