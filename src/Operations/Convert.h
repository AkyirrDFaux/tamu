bool ExecuteToType(OpContext &ctx)
{
    // ctx.Out = {0}
    // ctx.Args[0..7] = {1..8}

    switch (ctx.Op)
    {
    case Operations::ToBool:
    {
        bool b = GetAsNumber(ctx.Args[0]) > N(0.5);
        ctx.Out.SetCurrent(&b, 1, Types::Bool);
    }
    break;

    case Operations::ToByte:
    {
        uint8_t b = (uint8_t)GetAsNumber(ctx.Args[0]).RoundToInt();
        ctx.Out.SetCurrent(&b, 1, Types::Byte);
    }
    break;

    case Operations::ToInt:
    {
        int32_t i = GetAsNumber(ctx.Args[0]).RoundToInt();
        ctx.Out.SetCurrent(&i, 4, Types::Integer);
    }
    break;

    case Operations::ToNumber:
    {
        Number n = GetAsNumber(ctx.Args[0]);
        ctx.Out.SetCurrent(&n, sizeof(Number), Types::Number);
    }
    break;

    case Operations::ToVector2D:
    {
        Vector2D v(GetAsNumber(ctx.Args[0]),
                   GetAsNumber(ctx.Args[1]));
        ctx.Out.SetCurrent(&v, sizeof(Vector2D), Types::Vector2D);
    }
    break;

    case Operations::ToVector3D:
    {
        Vector3D v(GetAsNumber(ctx.Args[0]),
                   GetAsNumber(ctx.Args[1]),
                   GetAsNumber(ctx.Args[2]));
        ctx.Out.SetCurrent(&v, sizeof(Vector3D), Types::Vector3D);
    }
    break;

    case Operations::ToCoord2D:
    {
        // Flexible: Supports Vector2D+Vector2D OR Vector2D+Scalar
        if (ctx.Args[0].Type == Types::Vector2D)
        {
            Vector2D pos = *(Vector2D *)ctx.Args[0].Value;
            Coord2D c;
            if (ctx.Args[1].Type == Types::Vector2D)
                c = Coord2D(pos, *(Vector2D *)ctx.Args[1].Value);
            else
                c = Coord2D(pos, GetAsNumber(ctx.Args[1]));

            ctx.Out.SetCurrent(&c, sizeof(Coord2D), Types::Coord2D);
        }
    }
    break;

    case Operations::ToColour:
    {
        // Args 0,1,2 = R,G,B. Arg 3 = Alpha (Optional)
        uint8_t a = (ctx.Args[3].Value) ? (uint8_t)GetAsNumber(ctx.Args[3]).RoundToInt() : 255;
        ColourClass col((uint8_t)GetAsNumber(ctx.Args[0]).RoundToInt(),
                        (uint8_t)GetAsNumber(ctx.Args[1]).RoundToInt(),
                        (uint8_t)GetAsNumber(ctx.Args[2]).RoundToInt(), a);
        ctx.Out.SetCurrent(&col, sizeof(ColourClass), Types::Colour);
    }
    break;

    case Operations::ToColourHSV:
    {
        // Args 0,1,2 = H,S,V. Arg 3 = Alpha
        uint8_t a = (ctx.Args[3].Value) ? (uint8_t)GetAsNumber(ctx.Args[3]).RoundToInt() : 255;
        ColourClass col = ColourClass::FromHSV(GetAsNumber(ctx.Args[0]),
                                               GetAsNumber(ctx.Args[1]),
                                               GetAsNumber(ctx.Args[2]), a);
        ctx.Out.SetCurrent(&col, sizeof(ColourClass), Types::Colour);
    }
    break;

    default:
        return false;
    }
    return true;
}

bool ExecuteExtract(OpContext &ctx)
{
    // ctx.Out = {0} Output
    // ctx.Args[0] = {1} Composite Source
    // ctx.Args[1] = {2} Component Index

    if (!ctx.Args[0].Value || !ctx.Args[1].Value)
        return true;

    uint8_t componentIdx = (uint8_t)GetAsNumber(ctx.Args[1]).RoundToInt();
    Types sourceType = ctx.Args[0].Type;
    void *sourceVal = ctx.Args[0].Value;

    // --- Vector Extraction (X, Y, Z) ---
    if (sourceType == Types::Vector2D || sourceType == Types::Vector3D)
    {
        // Assuming Vector2D/3D components are stored as 'Number' (fixed-point)
        Number *v = (Number *)sourceVal;
        uint8_t max = (sourceType == Types::Vector2D) ? 2 : 3;

        if (componentIdx < max)
        {
            ctx.Out.SetCurrent(&v[componentIdx], sizeof(Number), Types::Number);
        }
    }
    // --- Colour Extraction (R, G, B, A) ---
    else if (sourceType == Types::Colour)
    {
        uint8_t *c = (uint8_t *)sourceVal;
        if (componentIdx < 4)
        {
            ctx.Out.SetCurrent(&c[componentIdx], 1, Types::Byte);
        }
    }
    // --- Coord2D Extraction (Offset, Rotation/Scale) ---
    else if (sourceType == Types::Coord2D)
    {
        Coord2D *coord = (Coord2D *)sourceVal;
        if (componentIdx == 0)
        {
            ctx.Out.SetCurrent(&coord->Offset, sizeof(Vector2D), Types::Vector2D);
        }
        else if (componentIdx == 1)
        {
            // Note: Check if your Coord2D stores the second part as Vector2D or Number
            ctx.Out.SetCurrent(&coord->Rotation, sizeof(Vector2D), Types::Vector2D);
        }
    }

    return true;
}