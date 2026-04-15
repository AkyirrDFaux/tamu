void ReadValue(const FlexArray &Input)
{
    // 1. Minimum Header Validation
    if (Input.Length < 5)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 2. Direct Pointer Mapping
    Reference *pID = (Reference *)(Input.Array + 1);
    uint16_t idSize = 4 + pID->PathLen();

    // 3. Object Lookup
    BaseClass *Object = Objects.At(*pID);
    if (!Object)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidID};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 4. Value Extraction
    FlexArray Data;
    if (pID->PathLen() > 0)
    {
        Bookmark Found = Object->Values.Find(*pID, true);
        Result FoundData = Found.Get();

        if (FoundData.Value != nullptr && FoundData.Length > 0)
        {
            // Efficiency: Reserve space for [Type(1)][Length(2)][Payload(N)]
            // This prevents multiple reallocs during the += chain
            Data.Reserve(1 + 2 + FoundData.Length);

            Data += FlexArray((char *)&FoundData.Type, 1);   // Pack Type
            Data += FlexArray((char *)&FoundData.Length, 2); // Pack Length
            Data += FlexArray((char *)FoundData.Value, FoundData.Length);
        }
    }

    // 5. Final Presence Check
    if (Data.Length == 0)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::NoValue};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 6. Assembly & Transmission
    char funcCode = (char)Functions::ReadValue;
    FlexArray Response(&funcCode, 1);

    Response += FlexArray((char *)pID, idSize); // Confirming which ID we read
    Response += Data;                           // Append the payload

    Chirp.Send(Response);
}

void WriteValue(const FlexArray &Input)
{
    // 1. Minimum Size Validation
    // [Func(1)] + [MinRef(4)] + ([Type(1)] + [Len(2)]) = 5 bytes minimum
    if (Input.Length < 5)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 2. Extract Reference (Offset 1)
    Reference *pID = (Reference *)(Input.Array + 1);
    uint16_t idSize = 4 + pID->PathLen();

    // 3. Resolve the Object
    BaseClass *Object = Objects.At(*pID);
    if (!Object)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidID};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 4. Extract Payload Header (Following the Reference)
    // Layout: [Ref...][Type(1)][Len(2)][Data...]

    Bookmark Index = Object->Values.Find(*pID, true);

    if (Input.Length == idSize + 1) // If no data then delete
    {
        Object->Values.Delete(Index.Index);
        SendUpdate(Object);
        return;
    }

    uint16_t payloadOffset = 1 + idSize;

    // Safety check to ensure we don't read past the end of Input
    if (payloadOffset + 3 > Input.Length)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::NoValue};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    Types pType = (Types)Input.Array[payloadOffset];
    uint16_t pLen;
    memcpy(&pLen, Input.Array + payloadOffset + 1, 2); //Unaligned!
    char *pData = Input.Array + payloadOffset + 3;

    // Final bounds check for the actual data payload
    if (payloadOffset + 3 + pLen > Input.Length)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::NoValue};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 5. Update Object State
    Object->Values.Set(pData, pLen, pType, *pID, Tri::Keep, Tri::Keep);
    if (Index.Index == INVALID_HEADER || Object->Values.HeaderArray[Index.Index].IsSetupCall())
        SendUpdate(Object);
    else
        ReadValue(Input);
}

void ReadName(const FlexArray &Input)
{
    // 1. Validation (Func + MinRef)
    if (Input.Length < 4)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 2. Map Reference
    Reference ID = Reference::Global(Input.Array[1], Input.Array[2], Input.Array[3]);

    // 3. Resolve Object
    BaseClass *Object = Objects.At(ID);
    if (!Object)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidID};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 4. Response: [ReadNameCode][ID][NameLen(1)][NameData]
    char funcCode = (char)Functions::ReadName;
    FlexArray Response(&funcCode, 1);
    Response += FlexArray((char *)ID.Data, 3);

    uint8_t nameLen = (Object->Name.Length > 255) ? 255 : (uint8_t)Object->Name.Length;
    Response += FlexArray((char *)&nameLen, 1);
    if (nameLen > 0)
        Response += FlexArray(Object->Name.Data, nameLen);

    Chirp.Send(Response);
}

void WriteName(const FlexArray &Input)
{
    // 1. Validation (Func + MinRef + NameLenByte)
    if (Input.Length < 5)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 2. Map Reference
    Reference ID = Reference::Global(Input.Array[1], Input.Array[2], Input.Array[3]);

    // 3. Resolve Object
    BaseClass *Object = Objects.At(ID);
    if (!Object)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidID};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    uint8_t incomingLen = (uint8_t)Input.Array[4];
    char *incomingData = Input.Array + 5;

    // Safety check for string length
    if (5 + incomingLen > Input.Length)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 5. Update the Name
    // Assuming Object->Name.Set or similar handles the raw buffer copy
    Object->Name = Text(incomingData, incomingLen);

    // 6. Confirmation Echo (Reuse ReadName logic)
    ReadName(Input);
}

void ReadInfo(const FlexArray &Input)
{
    // 1. Validation (Func + MinRef)
    if (Input.Length < 5)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 2. Map Reference
    Reference ID = Reference::Global(Input.Array[1], Input.Array[2], Input.Array[3]);

    // 3. Resolve Index
    int32_t Index = Objects.Search(ID);
    if (Index == -1)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidID};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 4. Response: [ReadInfoCode][ID][Flags(3 bytes)]
    char funcCode = (char)Functions::ReadInfo;
    FlexArray Response(&funcCode, 1);
    Response += FlexArray((char *)ID.Data, 3);

    // Append the 3 bytes of Info (Flags, RunPeriod, RunPhase)
    Response += FlexArray((char *)&Objects.Object[Index].Object->Flags, 3);

    Chirp.Send(Response);
}

void SetInfo(const FlexArray &Input)
{
    // 1. Validation (Func + MinRef (3) + 3 bytes of Info)
    if (Input.Length < 7)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidType};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 2. Map Reference
    Reference ID = Reference::Global(Input.Array[1], Input.Array[2], Input.Array[3]);

    // 3. Resolve Index
    int32_t Index = Objects.Search(ID);
    if (Index == -1)
    {
        char Resp[2] = {(char)Functions::Report, (char)Status::InvalidID};
        Chirp.Send(FlexArray(Resp, 2) += Input);
        return;
    }

    // 5. Update the Object and the Runner Registry
    BaseClass *Obj = Objects.Object[Index].Object;
    Obj->Flags = (FlagClass)Input.Array[4];
    Obj->RunPeriod = Input.Array[5];
    Obj->RunPhase = Input.Array[6];

    // Sync the runner info
    Objects.Object[Index].Info = {Obj->RunPeriod, Obj->RunPhase};

    // 6. Confirmation Echo
    // Reuse ReadInfo logic to send back the updated state
    ReadInfo(Input);
}