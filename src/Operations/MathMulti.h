bool ExecuteMath(OpContext &ctx)
{
    // ctx.Out = {0}
    // ctx.Args[0] = {1} First operand (determines target type)

    if (!ctx.Args[0].Value)
        return true;
    Types targetType = ctx.Args[0].Type;

    // --- 1. Vector 3D Lane ---
    if (targetType == Types::Vector3D)
    {
        Vector3D acc = *(Vector3D *)ctx.Args[0].Value;
        for (uint8_t i = 1; i < 8; i++)
        {
            if (ctx.Args[i].Type != Types::Vector3D || !ctx.Args[i].Value)
                break;
            Vector3D next = *(Vector3D *)ctx.Args[i].Value;

            if (ctx.Op == Operations::Add)
                acc = acc + next;
            else if (ctx.Op == Operations::Subtract)
                acc = acc - next;
            else if (ctx.Op == Operations::Multiply)
                acc = acc * next;
            else if (ctx.Op == Operations::Divide)
                acc = acc / next;
        }
        ctx.Out.SetCurrent(&acc, sizeof(Vector3D), Types::Vector3D);
    }
    // --- 2. Vector 2D Lane ---
    else if (targetType == Types::Vector2D)
    {
        Vector2D acc = *(Vector2D *)ctx.Args[0].Value;
        for (uint8_t i = 1; i < 8; i++)
        {
            if (ctx.Args[i].Type != Types::Vector2D || !ctx.Args[i].Value)
                break;
            Vector2D next = *(Vector2D *)ctx.Args[i].Value;

            if (ctx.Op == Operations::Add)
                acc = acc + next;
            else if (ctx.Op == Operations::Subtract)
                acc = acc - next;
            else if (ctx.Op == Operations::Multiply)
                acc = acc * next;
            else if (ctx.Op == Operations::Divide)
                acc = acc / next;
        }
        ctx.Out.SetCurrent(&acc, sizeof(Vector2D), Types::Vector2D);
    }
    // --- 3. Scalar Lane (Number, Int, Byte, Bool) ---
    else if (IsScalar(targetType))
    {
        Number acc = GetAsNumber(ctx.Args[0]);
        for (uint8_t i = 1; i < 8; i++)
        {
            if (!IsScalar(ctx.Args[i].Type) || !ctx.Args[i].Value)
                break;
            Number next = GetAsNumber(ctx.Args[i]);

            switch (ctx.Op)
            {
            case Operations::Add:
                acc += next;
                break;
            case Operations::Subtract:
                acc -= next;
                break;
            case Operations::Multiply:
                acc = acc * next;
                break;
            case Operations::Divide:
            { 
                acc = (next != N(0.0)) ? acc / next : N(0.0);
            }
            break;
            case Operations::Max:
                if (next > acc)
                    acc = next;
                break;
            case Operations::Min:
                if (next < acc)
                    acc = next;
                break;
            case Operations::Modulo:
            { 
                if (next != N(0.0))
                {
                    Number q = acc / next;
                    Number f = q.ToInt();
                    if (f > q)
                        f = f - N(1.0);
                    acc = acc - (next * f);
                }
                else
                {
                    acc = N(0.0);
                }
            }
            break;
            default:
                break;
            }
        }
        StoreScalar(ctx.Out, acc, targetType);
    }

    return true;
}

bool ExecuteVectorMath(OpContext &ctx)
{
    // Handle 2D path
    if (ctx.Args[0].Type == Types::Vector2D)
    {
        Vector2D A = *(Vector2D*)ctx.Args[0].Value;
        
        switch (ctx.Op)
        {
            case Operations::Magnitude: {
                Number res = A.Length();
                ctx.Out.SetCurrent(&res, sizeof(Number), Types::Number);
                break;
            }
            case Operations::Angle: {
                Number res = A.ToAngle();
                ctx.Out.SetCurrent(&res, sizeof(Number), Types::Number);
                break;
            }
            case Operations::Normalize: {
                Number len = A.Length();
                Vector2D res = (len.Value > 0) ? A * (Number(1) / len) : Vector2D(0, 0);
                ctx.Out.SetCurrent(&res, sizeof(Vector2D), Types::Vector2D);
                break;
            }
            case Operations::DotProduct: {
                if (ctx.Args[1].Type == Types::Vector2D) {
                    Number res = A.DotProduct(*(Vector2D*)ctx.Args[1].Value);
                    ctx.Out.SetCurrent(&res, sizeof(Number), Types::Number);
                }
                break;
            }
            default: break;
        }
    }
    // Handle 3D path
    else if (ctx.Args[0].Type == Types::Vector3D)
    {
        Vector3D A = *(Vector3D*)ctx.Args[0].Value;

        switch (ctx.Op)
        {
            case Operations::Magnitude: {
                Number res = A.Length();
                ctx.Out.SetCurrent(&res, sizeof(Number), Types::Number);
                break;
            }
            case Operations::Normalize: {
                Vector3D res = A.Normalize();
                ctx.Out.SetCurrent(&res, sizeof(Vector3D), Types::Vector3D);
                break;
            }
            case Operations::DotProduct: {
                if (ctx.Args[1].Type == Types::Vector3D) {
                    Number res = A.DotProduct(*(Vector3D*)ctx.Args[1].Value);
                    ctx.Out.SetCurrent(&res, sizeof(Number), Types::Number);
                }
                break;
            }
            case Operations::CrossProduct: {
                if (ctx.Args[1].Type == Types::Vector3D) {
                    Vector3D res = A.CrossProduct(*(Vector3D*)ctx.Args[1].Value);
                    ctx.Out.SetCurrent(&res, sizeof(Vector3D), Types::Vector3D);
                }
                break;
            }
            default: break;
        }
    }

    return true;
}