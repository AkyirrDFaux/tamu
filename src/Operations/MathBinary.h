bool ExecuteCompare(OpContext &ctx)
{
    // ctx.Out = {0} Output
    // ctx.Args[0] = {1} Left Operand
    // ctx.Args[1] = {2} Right Operand

    // Only compare if both are scalars; complex types (Vectors/Colors)
    // don't have a natural "Greater Than" logic here.
    if (!IsScalar(ctx.Args[0].Type) || !IsScalar(ctx.Args[1].Type))
        return true;

    Number Left = GetAsNumber(ctx.Args[0]);
    Number Right = GetAsNumber(ctx.Args[1]);
    bool comparison = false;

    switch (ctx.Op)
    {
    case Operations::IsEqual:
        comparison = (Left == Right);
        break;
    case Operations::IsGreater:
        comparison = (Left > Right);
        break;
    case Operations::IsSmaller:
        comparison = (Left < Right);
        break;
    default:
        return true;
    }

    // Always output as a Bool type
    ctx.Out.SetCurrent(&comparison, sizeof(bool), Types::Bool);
    return true;
}

bool ExecuteLogic(OpContext &ctx)
{
    // ctx.Args[0] = {1} Input A (Bool)
    // ctx.Args[1] = {2} Input B (Bool) / Memory Slot
    // ctx.Args[2] = {3} Memory Slot for SR Latch
    
    bool A = (ctx.Args[0].Value) ? *(bool*)ctx.Args[0].Value : false;
    bool B = (ctx.Args[1].Value) ? *(bool*)ctx.Args[1].Value : false;
    bool result = false;

    switch (ctx.Op)
    {
        case Operations::And:
            result = A && B;
            break;

        case Operations::Or:
            result = A || B;
            break;

        case Operations::Not:
            result = !A;
            break;

        case Operations::Edge: {
            // Edge Detection: A is current signal, ctx.Args[1] is the previous state
            // We use the ArgMark[1] to persist the state across frames
            bool lastState = (ctx.Args[1].Value) ? *(bool*)ctx.Args[1].Value : false;
            
            // Rising Edge detection (False -> True)
            result = (A && !lastState);
            
            // Store the current state as the 'lastState' for the next run
            ctx.ArgMarks[1].SetCurrent(&A, sizeof(bool), Types::Bool);
            break;
        }

        case Operations::SetReset: {
            // A = Set, B = Reset
            // We need a third slot for the persistent "State"
            bool state = (ctx.Args[2].Value) ? *(bool*)ctx.Args[2].Value : false;
            
            if (B) {
                state = false; // Reset has priority
            } else if (A) {
                state = true;  // Set
            }
            
            result = state;
            // Update the persistent state in the ArgMark slot
            ctx.ArgMarks[2].SetCurrent(&state, sizeof(bool), Types::Bool);
            break;
        }
        
        default: break;
    }

    ctx.Out.SetCurrent(&result, sizeof(bool), Types::Bool);
    return true;
}