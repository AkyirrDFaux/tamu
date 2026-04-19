bool RunOperation(const Bookmark &Index)
{
    Result opRes = Index.GetThis();
    if (opRes.Type != Types::Operation || !opRes.Value)
        return true;

    Operations OpCode = *(Operations *)opRes.Value;
    if (OpCode == Operations::None)
        return true;

    OpContext ctx;
    ctx.Op = OpCode;
    ctx.Out = Index.Child();

    if (ctx.Out.Index == INVALID_HEADER)
        return true;

    // Prefetch both Data (Result) and Pointer (Bookmark)
    Bookmark inputMark = ctx.Out.Next();
    for (uint8_t i = 0; i < 8; i++)
    {
        if (inputMark.Index != INVALID_HEADER)
        {
            ctx.Args[i] = inputMark.GetThis();
            ctx.ArgMarks[i] = inputMark;
            inputMark = inputMark.Next();
        }
        else
        {
            ctx.Args[i] = Result();
            ctx.ArgMarks[i] = Bookmark(); // Invalid bookmark
        }
    }

    // 3. Execute the Operation Logic
    bool Done = true;
    switch (OpCode)
    {
    case Operations::Set:
        Done = ExecuteSet(ctx);
        break;

    case Operations::Delete:
        Done = ExecuteDelete(ctx);
        break;

    case Operations::Extract:
        Done = ExecuteExtract(ctx);
        break;

    // Comparisons
    case Operations::IsEqual:
    case Operations::IsGreater:
    case Operations::IsSmaller:
        Done = ExecuteCompare(ctx);
        break;

    // Scalar Math
    case Operations::Add:
    case Operations::Subtract:
    case Operations::Multiply:
    case Operations::Divide:
    case Operations::Power:
    case Operations::Absolute:
    case Operations::Min:
    case Operations::Max:
    case Operations::Modulo:
        Done = ExecuteMath(ctx);
        break;

    case Operations::Random:
        Done = ExecuteRandom(ctx);
        break;

    // Vector & Coordinate Math
    case Operations::Magnitude:
    case Operations::Angle:
    case Operations::Normalize:
    case Operations::DotProduct:
    case Operations::CrossProduct:
        Done = ExecuteVectorMath(ctx);
        break;

    // Type Conversion / Casting
    case Operations::ToBool:
    case Operations::ToByte:
    case Operations::ToInt:
    case Operations::ToNumber:
    case Operations::ToVector2D:
    case Operations::ToVector3D:
    case Operations::ToCoord2D:
    case Operations::ToColour:
    case Operations::ToColourHSV:
        Done = ExecuteToType(ctx);
        break;

    // Mapping & Interpolation
    case Operations::Clamp:
        Done = ExecuteClamp(ctx);
        break;
    case Operations::Deadzone:
        Done = ExecuteDeadzone(ctx);
        break;
    case Operations::LinInterpolation:
        Done = ExecuteLerp(ctx);
        break;

    // Waveforms / Oscillators
    case Operations::Sine:
    case Operations::Square:
    case Operations::Sawtooth:
        Done = ExecuteWaveforms(ctx);
        break;

    // Logic & Signals
    case Operations::And:
    case Operations::Or:
    case Operations::Not:
    case Operations::Edge:
    case Operations::SetReset:
        Done = ExecuteLogic(ctx);
        break;

    // Program Flow & Lifecycle
    case Operations::Delay:
        Done = ExecuteDelay(ctx);
        break;
    case Operations::IfSwitch:
        Done = ExecuteIfSwitch(ctx);
        break;
    case Operations::SetRunOnce:
    case Operations::SetRunLoop:
        Done = ExecuteSetRun(ctx);
        break;

    case Operations::SetReference:
        Done = true;
        break;
    case Operations::Save:
        Done = ExecuteSave(ctx);
        break;
    default:
        ReportError(Status::InvalidValue);
        return true;
    }

    // 4. Original Broadcast Logic: Push Result to Linked References
    // We use ctx.Out because it's the exact same bookmark as outMark
    Result Payload = ctx.Out.Get();
    if (Payload.Length == 0 || !Payload.Value)
        return Done;

    // Create a local stack copy of the result
    // 16 bytes covers Number, Vector3D, ColourClass, etc.
    uint8_t safeBuffer[32];
    uint8_t copySize = (Payload.Length > 32) ? 32 : Payload.Length;
    memcpy(safeBuffer, Payload.Value, copySize);

    Types safeType = Payload.Type;
    uint8_t safeLen = Payload.Length;

    // Now we broadcast from safeBuffer, NOT from Payload.Value
    Bookmark linkMark = ctx.Out.Child();
    while (linkMark.Index != INVALID_HEADER)
    {
        Result Link = linkMark.Get();
        if (Link.Length == 0 || Link.Type != Types::Reference)
            break;

        Reference TargetRef = *(Reference *)Link.Value;
        if (TargetRef.IsGlobal())
        {
            BaseClass *TargetObj = Objects.At(TargetRef);
            if (TargetObj != nullptr)
                TargetObj->Values.Set(safeBuffer, safeLen, safeType, TargetRef);
        }
        else // Skip long search
            Index.Map->Set(safeBuffer, safeLen, safeType, TargetRef);

        linkMark = linkMark.Next();
    }

    return Done;
}