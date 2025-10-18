void Run(ByteArray &Input)
{
    // Serial.println(Input.ToHex());
    ByteArray Function = Input.ExtractPart();
    if (Function.Type() == Types::Function)
    {
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
        else if (Function == Functions::ReadFile)
            ReadFile(Input);
        else if (Function == Functions::SaveAll)
            SaveAll(Input);
        else if (Function == Functions::RunFile)
            RunFile(Input);
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
    Chirp.Send(ByteArray(Status::InvalidType) << Input);
}

void ReadDatabase(ByteArray &Input)
{
    Serial.println(Objects.ContentDebug());
}

void Refresh(ByteArray &Input)
{
    for (uint32_t Index = 1; Index <= Objects.Allocated; Index++)
    {
        if (!Objects.IsValid(IDClass(Index)))
            continue;
        Chirp.Send(ByteArray(Functions::ReadObject) << ByteArray(*Objects[IDClass(Index)]));
    }
    Chirp.Send(ByteArray(Status::OK));
}

BaseClass *CreateObject(Types Type, bool New, IDClass ID, FlagClass Flags)
{
    switch (Type)
    {
    // Port is auto
    case Types::Shape2D:
        return new Shape2DClass(ID, Flags);
    // Byte
    // Bool
    //  Type is internal
    //  Function is internal
    //  Flags is internal
    //  Status is internal
    case Types::Board:
        return new BoardClass(ID, Flags);
    case Types::Fan:
        return new FanClass(ID, Flags);
    case Types::LEDSegment:
        return new LEDSegmentClass(ID, Flags);
    case Types::Texture1D:
        return new Texture1D(ID, Flags);
    case Types::LEDStrip:
        return new LEDStripClass(ID, Flags);
    case Types::Geometry2D:
        return new Geometry2DClass(ID, Flags);
    //GeometryOperation
    case Types::Texture2D:
        return new Texture2D(ID, Flags);
    case Types::Display:
        return new DisplayClass(ID, Flags);
    case Types::AccGyr:
        return new GyrAccClass(GyrAccs::Undefined, ID, Flags);
    case Types::Input:
        return new InputClass(Inputs::Undefined, ID, Flags);
    //case Types::Operation:
    //    return new Variable<Operations>(Operations::None, ID, Flags);
    //case Types::Program:
    //    return new Program(New, ID, Flags);
    // Integer
    // Time
    // Number
    // ID is internal
    // Colour
    // Vector2D
    // Coord2D
    // Text
    //  IDList is internal
    default:
        ReportError(Status::InvalidType, "Type not allowed for BaseClass creation:" + String((uint8_t)Type));
        return nullptr;
    }
}

void CreateObject(ByteArray &Input)
{
    BaseClass *Object = nullptr;
    ByteArray Type = Input.ExtractPart();
    ByteArray ID = Input.ExtractPart();
    if (Type.Type() == Types::Type && ID.Type() == Types::ID)
    {
        if (!Objects.IsValid(ID)) // Check for ID colision
            Object = CreateObject(Type, true, ID);
        else
        {
            Chirp.Send(ByteArray(Status::InvalidID) << Input);
            return;
        }
    }
    else if (Type.Type() == Types::Type)
        Object = CreateObject((Types)Type);
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
    ByteArray Type = Input.ExtractPart();
    ByteArray ID = Input.ExtractPart();
    ByteArray Flags = Input.ExtractPart();
    ByteArray Name = Input.ExtractPart();
    ByteArray Modules = Input.ExtractPart();
    ByteArray Value = Input.ExtractPart();
    if (Type.Type() == Types::Type && ID.Type() == Types::ID)
    {
        if (!Objects.IsValid(ID)) // Check for ID colision
            Object = CreateObject(Type, false, ID);
        else if (Objects[ID]->Type == (Types)Type) // Overwrite the object, since the type and ID are same
            Object = Objects[ID];
        else
        {
            Chirp.Send(ByteArray(Status::InvalidID) << Input);
            return;
        }
    }
    else
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    if (Flags.Type() == Types::Flags)
        Object->Flags = Flags.As<FlagClass>();
    if (Name.Type() == Types::Text)
        Object->Name = Name.As<String>();
    if (Modules.Type() == Types::IDList)
        Object->Modules = Modules.As<IDList>(); // Fixed, careful
    if (Value.Type() == Object->Type)
        Object->SetValue(Value);

    Chirp.Send(ByteArray(Functions::LoadObject) << ByteArray(*Object));
}

void DeleteObject(ByteArray &Input)
{
    ByteArray ID = Input.ExtractPart();
    if (ID.Type() != Types::ID || !Objects.IsValid(ID))
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
    ByteArray ID = Input.ExtractPart();
    ByteArray Modules = Input.ExtractPart();

    if (ID.Type() != Types::ID || !Objects.IsValid(ID))
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }
    else if (Objects[ID]->Flags == Flags::Auto)
    {
        Chirp.Send(ByteArray(Status::AutoObject) << Input);
        return;
    }
    else if (Modules.Type() != Types::IDList)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }
    Objects[ID]->Modules = Modules.As<IDList>();
    Chirp.Send(ByteArray(Functions::SetModules) << ID << Modules);
};

void SetFlags(ByteArray &Input)
{
    ByteArray ID = Input.ExtractPart();
    ByteArray Flags = Input.ExtractPart();

    if (ID.Type() != Types::ID || !Objects.IsValid(ID))
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }
    else if (Objects[ID]->Flags == Flags::Auto)
    {
        Chirp.Send(ByteArray(Status::AutoObject) << Input);
        return;
    }
    else if (Flags.Type() != Types::Flags)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }
    Objects[ID]->Flags = Flags.As<FlagClass>();
    //if (Objects[ID]->Type == Types::Program && Objects[ID]->Flags == Flags::RunOnce)
    //    Objects[ID]->As<Program>()->Counter = 0;
    Chirp.Send(ByteArray(Functions::SetFlags) << ID << Flags);
};

void WriteName(ByteArray &Input)
{
    ByteArray ID = Input.ExtractPart();
    ByteArray Name = Input.ExtractPart();

    if (ID.Type() != Types::ID || !Objects.IsValid(ID))
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }
    else if (Objects[ID]->Flags == Flags::Auto)
    {
        Chirp.Send(ByteArray(Status::AutoObject) << Input);
        return;
    }
    else if (Name.Type() != Types::Text)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }
    Objects[ID]->Name = Name.As<String>();
    Chirp.Send(ByteArray(Functions::WriteName) << ID << Name);
};

void ReadName(ByteArray &Input)
{
    ByteArray ID = Input.ExtractPart();
    if (ID.Type() != Types::ID || !Objects.IsValid(ID))
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    Chirp.Send(ByteArray(Functions::ReadName) << ID << ByteArray(Objects[ID]->Name));
};
