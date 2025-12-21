BaseClass::BaseClass(IDClass NewID, FlagClass NewFlags)
{
    Flags = NewFlags;
    Objects.Register(this, NewID);
};

BaseClass::~BaseClass()
{
    // Remove references to this from all modules
    for (int32_t Index = Modules.FirstValid(); Index < Modules.Length; Modules.Iterate(&Index))
    {
        if (Modules[Index]->ReferencesCount > 0)
            Modules[Index]->ReferencesCount--;

        if (Modules[Index]->Flags == Flags::Auto) // If modules are static (directly tied) deconstruct them too
            delete Modules[Index];
    }
    // Remove this as a module from all references
    for (int32_t Index = 0; Index < Objects.Allocated; Index++)
    {
        if (Objects.IsValid(Index) == false)
            continue;
        Objects[Index]->Modules.Remove(this);
    }

    // Clear lists, unregister this
    Modules.RemoveAll();
    Objects.Unregister(this);
};

ByteArray BaseClass::GetValue(int32_t Value) const
{
    ByteArray Data = ByteArray();
    Types Type = Types::Undefined;
    for (uint32_t Index = max((int32_t)0,Value-1); Index < Values.Length; Index++)
    {
        Type = Values.Type[Index];
        if (Type == Types::Text)
            Data = Data << ByteArray(*Values.At<String>(Index));
        else if (Type == Types::IDList)
            Data = Data << ByteArray(*Values.At<IDList>(Index));
        #if USE_FIXED_POINT == 1
        else if (Type == Types::Number)
            Data = Data << ByteArray(*Values.At<Number>(Index));
        else if (Type == Types::Vector2D)
            Data = Data << ByteArray(*Values.At<Vector2D>(Index));
        else if (Type == Types::Vector3D)
            Data = Data << ByteArray(*Values.At<Vector3D>(Index));
        else if (Type == Types::Coord2D)
            Data = Data << ByteArray(*Values.At<Coord2D>(Index));
        #endif
        else if (Type == Types::Undefined)
            Data = Data << ByteArray((char *)&Type, sizeof(Types));
        else
            Data = Data << ByteArray((char *)&Type, sizeof(Types)) << ByteArray((char *)Values.Data[Index], GetDataSize(Type));
        
        if (Value != 0)
            break;
    }
    return Data;
};

bool BaseClass::SetValue(ByteArray &Input, uint8_t Value)
{
    ByteArray Part = Input.ExtractPart();
    int32_t Index = max(Value-1,0);
    while (Part.Length > 0 && (Value == 0 || (Value-1 == Index && Value != 0)))
    {
        if (Part.Type() == Types::Undefined)
        {
            Part = Input.ExtractPart();
            Index++;
            continue;
        }

        //TODO String, IDList, Fixed-point
        if (Values.IsValid(Index,Part.Type()) == false) // Prepare for copying
        {
            if (Values.IsValid(Index)) // Already occupied, but different type
                Values.Delete(Index);
            else //Possibly not allocated yet
                Values.Expand(Index + 1);
            
            if(Part.Type() == Types::Text)
                Values.Data[Index] = new String;
            else if(Part.Type() == Types::IDList)
                Values.Data[Index] = new IDList;
            else
                Values.Data[Index] = new char[Part.SizeOfData()]; 

            Values.Type[Index] = Part.Type();
        }

        if(Part.Type() == Types::Text)
            *Values.At<String>(Index) = Part.As<String>();
        else if(Part.Type() == Types::IDList)
            *Values.At<IDList>(Index) = Part.As<IDList>();
        #if USE_FIXED_POINT == 1
        else if (Part.Type() == Types::Number)
            *Values.At<Number>(Index) = Part.As<Number>();
        else if (Part.Type() == Types::Vector2D)
            *Values.At<Vector2D>(Index) = Part.As<Vector2D>();
        else if (Part.Type() == Types::Vector3D)
            *Values.At<Vector3D>(Index) = Part.As<Vector3D>();
        else if (Part.Type() == Types::Coord2D)
            *Values.At<Coord2D>(Index) = Part.As<Coord2D>();    
        #endif
        else
            memcpy(Values.Data[Index],Part.Array + sizeof(Types), Part.SizeOfData());

        Part = Input.ExtractPart();
        Index++;
    }
    Setup();
    return true;
}