void ReadValue(const ByteArray &Input)
{
    Getter<Reference> Ref = Input.Get<Reference>({1});
    if (!Ref.Success)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    BaseClass *Object = Objects.At(Ref);
    if (!Object)
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    ByteArray Data;
    // If Path exists in the Reference, copy just that value.
    // Otherwise, Data = Object->Values (uses the Copy Constructor).
    if (Ref.Value.Location.Length > 0)
    {
        Data = Object->Values.Copy(Ref.Value.Location);
    }
    else
    {
        Data = Object->Values;
    }

    if (Data.Length == 0)
    {
        Chirp.Send(ByteArray(Status::NoValue) << Input);
        return;
    }

    Chirp.Send(ByteArray(Functions::ReadValue) << ByteArray(Ref) << Data);
}

void WriteValue(const ByteArray &Input)
{
    // 1. Resolve the Reference (Target Object + Target Path)
    // Segment {0} is Function, Segment {1} is Reference
    Getter<Reference> Ref = Input.Get<Reference>({1});

    if (!Ref.Success)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    // 2. Resolve the Object Pointer from the Register
    BaseClass *Object = Objects.At(Ref);
    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    // 3. Locate the SINGLE New Data payload at Index {2}
    FindResult Payload = Input.Find({2});

    // Guard: If there is no data at index 2, or if the header is invalid, abort.
    if (Payload.Header == -1)
    {
        Chirp.Send(ByteArray(Status::NoValue) << Input);
        return;
    }

    // 4. Update the Object's internal Values (Strictly one entry)
    const Header &DataHead = Input.HeaderArray[Payload.Header];

    // Note: We use Ref.Value.Location (the internal path) to ensure 
    // we are writing to the exact node intended by the Reference.
    Object->Values.Set(
        Payload.Value, 
        DataHead.Length, 
        DataHead.Type, 
        Ref.Value.Location
    );

    // 5. Confirmation Echo
    // We send back the current state of that specific location to confirm the write.
    Chirp.Send(
        ByteArray(Functions::ReadValue) 
        << ByteArray(Ref.Value) 
        << Object->Values.Copy(Ref.Value.Location)
    );
}

void ReadName(const ByteArray &Input)
{
    // 1. Resolve the Reference using the optimized Getter
    Getter<Reference> Ref = Input.Get<Reference>({1});

    // 2. Validate type alignment (merges Type check and Existence)
    if (!Ref.Success)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    // 3. Resolve the pointer from the registry
    BaseClass *Object = Objects.At(Ref);

    // 4. Verify the object exists
    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    // 5. Send the Name back
    // We pass the Reference back so the Flutter app knows which name belongs to which ID
    Chirp.Send(ByteArray(Functions::ReadName) << ByteArray(Ref.Value) << ByteArray(Object->Name));
}

void WriteName(const ByteArray &Input)
{
    // 1. Resolve the Reference and the New Name
    Getter<Reference> Ref = Input.Get<Reference>({1});
    Getter<Text> NewName = Input.Get<Text>({2});

    // 2. Validate types and existence in one go
    if (!Ref.Success || !NewName.Success)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    // 3. Resolve the pointer from the registry
    BaseClass *Object = Objects.At(Ref);

    // 4. Verify the object exists
    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    // 5. Update the object's name
    // Assuming Text has an assignment operator or internal buffer update
    Object->Name = NewName;

    // 6. Confirm the update
    // Send back the Function, the Reference, and the confirmed Name
    Chirp.Send(ByteArray(Functions::ReadName) << ByteArray(Ref.Value) << ByteArray(Object->Name));
}

void ReadFlags(const ByteArray &Input)
{
    // 1. Resolve the Reference from Index 1
    Getter<Reference> Ref = Input.Get<Reference>({1});

    // 2. Validate type alignment
    if (!Ref.Success)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    // 3. Resolve the pointer from the registry
    BaseClass *Object = Objects.At(Ref);

    // 4. Verify the object exists
    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    // 5. Respond with the specific object's flags
    // Send: Function + Original Reference + Current FlagClass
    Chirp.Send(ByteArray(Functions::ReadFlags) << ByteArray(Ref.Value) << ByteArray(Object->Flags));
}

void SetFlags(const ByteArray &Input)
{
    // 1. Resolve the Reference and the New Flags
    Getter<Reference> Ref = Input.Get<Reference>({1});
    Getter<FlagClass> Flags = Input.Get<FlagClass>({2});

    // 2. Validate types and existence
    if (!Ref.Success || !Flags.Success)
    {
        Chirp.Send(ByteArray(Status::InvalidType) << Input);
        return;
    }

    // 3. Resolve the object pointer
    BaseClass *Object = Objects.At(Ref);

    if (Object == nullptr)
    {
        Chirp.Send(ByteArray(Status::InvalidID) << Input);
        return;
    }

    // 4. Update the Flags
    Object->Flags = Flags;

    // 5. Special Logic: Reset counter for RunOnce programs
    // We use the object's Values.Set to ensure the reset is tracked internally
    /*if (Object->Type == ObjectTypes::Program && Object->Flags == Flags::RunOnce)
    {
        int32_t ResetValue = 0;
        Object->Values.Set(&ResetValue, sizeof(int32_t), Types::Integer, {1});
    }*/

    // 6. Confirm the update
    // Return Function, Reference, and the newly applied FlagClass
    Chirp.Send(ByteArray(Functions::ReadFlags) << ByteArray(Ref.Value) << ByteArray(Object->Flags));
}