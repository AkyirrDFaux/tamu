class DisplayClass : public BaseClass
{
private:
    Number *Overlay = nullptr;
    int32_t MaxLength = 0;
    void ManageOverlay(int32_t RequiredLength);

public:
#if defined O_LAYOUT_USED
    uint8_t *Layout = nullptr;
#endif
    PortNumber CurrentLink = -1;
    uint8_t *LEDs = nullptr;

    DisplayClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});
    ~DisplayClass();

    void Setup(uint16_t Index);
    bool Run();

    bool Connect(BaseClass *Object = nullptr, int32_t Index = -1);
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, uint16_t Index)
    {
        static_cast<DisplayClass *>(Base)->Setup(Index);
    }
    static bool RunBridge(BaseClass *Base) { return static_cast<DisplayClass *>(Base)->Run(); }

    static constexpr VTable Table = {
        .Setup = DisplayClass::SetupBridge,
        .Run = DisplayClass::RunBridge};

    void RenderGeometry(uint16_t NodeIdx, int32_t Length, Vector2D DisplaySize,
                        Coord2D Transform, bool Mirrored, Number *Overlay);
    Number CalculateShapeAlpha(Geometries Type, Vector2D P, Vector2D S, Number F);
    void RenderTexture(uint16_t NodeIdx, int32_t Length, Vector2D DisplaySize,
                       Coord2D Transform, bool Mirrored, Number *Overlay);
};

DisplayClass::DisplayClass(const Reference &ID, FlagClass Flags, RunInfo Info)
    : BaseClass(&Table, ID, Flags, Info)
{
    Type = ObjectTypes::Display;
    Name = "Display";

    uint16_t cursor = 0;

    // --- Branch {0}: Hardware Definition (Depth 0) ---
    Displays initType = Displays::Undefined;
    Values.Set(&initType, sizeof(Displays), Types::Display, cursor++, 0, Tri::Reset, Tri::Set); // {0}

    // --- Branch {0} Children (Depth 1) ---
    PortNumber initPort = -1;
    Values.Set(&initPort, sizeof(PortNumber), Types::PortNumber, cursor++, 1, Tri::Reset, Tri::Set); // {0, 0}

    int32_t initLen = 0;
    Values.Set(&initLen, sizeof(int32_t), Types::Integer, cursor++, 1, Tri::Reset, Tri::Set); // {0, 1}

    Vector2D initSize(0, 0);
    Values.Set(&initSize, sizeof(Vector2D), Types::Vector2D, cursor++, 1, Tri::Reset, Tri::Reset); // {0, 2}

    Number initRatio = N(1.0);
    Values.Set(&initRatio, sizeof(Number), Types::Number, cursor++, 1, Tri::Reset, Tri::Reset); // {0, 3}

    // --- Branch {1}: Control (Depth 0 - Sibling of {0}) ---
    uint8_t initBright = 20;
    Values.Set(&initBright, sizeof(uint8_t), Types::Byte, cursor++, 0, Tri::Reset, Tri::Reset); // {1}

    // --- Branch {1} Children (Depth 1) ---
    Coord2D initOffset(0, 0, 0);
    Values.Set(&initOffset, sizeof(Coord2D), Types::Coord2D, cursor++, 1, Tri::Reset, Tri::Reset); // {1, 0}

    bool initFalse = false;
    Values.Set(&initFalse, sizeof(bool), Types::Bool, cursor++, 1, Tri::Reset, Tri::Reset); // {1, 1}
    Values.Set(&initFalse, sizeof(bool), Types::Bool, cursor++, 1, Tri::Reset, Tri::Reset); // {1, 2}
    Values.Set(&initFalse, sizeof(bool), Types::Bool, cursor++, 1, Tri::Reset, Tri::Reset); // {1, 3}
}

DisplayClass::~DisplayClass()
{
    Disconnect();
    if (Overlay)
        delete[] Overlay;
};

bool DisplayClass::Connect(BaseClass *Object, int32_t Index)
{
    if (!Object)
        Object = this;

    // Hardcoded index for {0, 0} based on the constructor's linear build.
    // Child of Root(0) is index 1.
    uint16_t linkIdx = 1;
    Result linkRes = Values.Get(linkIdx);

    if (!linkRes.Value)
        return false;

    // 1. Daisy-chain (Reference)
    if (linkRes.Type == Types::Reference)
    {
        BaseClass *Target = Objects.At(*(Reference *)linkRes.Value);
        // Direct cast and recursion
        if (Target && Target->Type == ObjectTypes::Display)
            return ((DisplayClass *)Target)->Connect(Object, Index + 1);

        return false;
    }

    // 2. Direct Board Connection (PortNumber)
    // We only reach this if the first display in the chain is hit
    PortNumber Port = *(PortNumber *)linkRes.Value;
    if (Port.IsValid() && Board.ConnectLED(Object, Port, Index + 1))
    {
        if (Object->Type == ObjectTypes::Display)
            ((DisplayClass *)Object)->CurrentLink = Port;
        return true;
    }

    return false;
}

bool DisplayClass::Disconnect()
{
    if (CurrentLink.IsValid() == false)
        return false;

    if (Board.DisconnectLED(this, CurrentLink))
    {
        CurrentLink = -1;
        return true;
    }
    return false;
}

void DisplayClass::Setup(uint16_t Index)
{
    bool HardwareChanged = false;

    // Direct Index Access (O(1)) based on Linear Constructor:
    // 0: Type {0}
    // 1: Port {0, 0}
    // 2: Length {0, 1}
    // 3: Size {0, 2}

    if (Index == 0) // Displays::Type changed
    {
        Result res = Values.Get(0);
        if (res.Value)
        {
            Displays Type = *(Displays *)res.Value;
            int32_t newLen = 0;
            Vector2D newSize(0, 0);

            if (Type == Displays::GenericLEDMatrix || Type == Displays::GenericLEDMatrixWeave)
            {
                newLen = 256;
                newSize = Vector2D(16, 16);
#if defined O_LAYOUT_USED
                Layout = nullptr;
#endif
            }

#if defined O_VYSI_V1_0
            if (Type == Displays::Vysi_v1_0)
            {
                newLen = 86;
                newSize = Vector2D(11, 10);
                Layout = LayoutVysiv1_0;
            }
#endif

            if (newLen > 0)
            {
                // SetExisting is safer here since the nodes already exist
                Values.SetExisting(&newLen, sizeof(int32_t), Types::Integer, 2);    // {0, 1}
                Values.SetExisting(&newSize, sizeof(Vector2D), Types::Vector2D, 3); // {0, 2}
                HardwareChanged = true;
            }
        }
    }
    // If Port {0,0} or Length {0,1} were manually updated via Serial/UI
    else if (Index == 1 || Index == 2)
    {
        HardwareChanged = true;
    }

    if (HardwareChanged)
    {
        Result portRes = Values.Get(1); // {0, 0}
        Result lenRes = Values.Get(2);  // {0, 1}

        if (lenRes.Value)
        {
            // Re-allocate or resize the drawing buffer
            ManageOverlay(*(int32_t *)lenRes.Value);
        }

        // Only cycle hardware connection if we have a valid port assignment
        if (portRes.Value && (*(PortNumber *)portRes.Value).IsValid())
        {
            Disconnect();
            Connect();
        }
    }
}

void DisplayClass::ManageOverlay(int32_t RequiredLength)
{
    if (RequiredLength <= 0)
        return;
    if (RequiredLength > MaxLength || Overlay == nullptr)
    {
        Number *NewOverlay = new Number[RequiredLength]();
        if (!NewOverlay)
            return;

        if (Overlay != nullptr)
            delete[] Overlay;
        Overlay = NewOverlay;
        MaxLength = RequiredLength;
    }
}

bool DisplayClass::Run()
{
    // Step 1: Direct Index Access (Zero Path Parsing)
    // 0 = Type, 1 = Port, 2 = Length, 3 = Size, 4 = Ratio
    // 5 = Bright, 6 = Offset, 7 = Mirror, 8 = WrapX, 9 = WrapY

    Result lenRes = Values.Get(2);
    Result sizeRes = Values.Get(3);
    Result brightRes = Values.Get(5);

    if (!lenRes.Value || !sizeRes.Value || !brightRes.Value || !LEDs || !Overlay)
        return true;

    int32_t Length = *(int32_t *)lenRes.Value;
    if (Length <= 0)
        return true;

    Vector2D Size = *(Vector2D *)sizeRes.Value;
    uint8_t Bright = *(uint8_t *)brightRes.Value;
    Coord2D Offset = *(Coord2D *)Values.Get(6).Value;
    bool Mirrored = *(bool *)Values.Get(7).Value;

    // Pre-calculate transform
    Coord2D BaseTransform = Coord2D(Size * N(0.5) - Vector2D(N(0.5), N(0.5)), Vector2D(0)).Join(Offset);

    // Efficient clear
    memset(LEDs, 0, Length * 3);
    memset((void *)Overlay, 0, Length * sizeof(Number));

    // Step 2: Iterate Render Stack
    // Based on constructor: {0} is idx 0, {1} is idx 5.
    // Therefore, Branch {2} (Render Stack) starts after the last child of {1}.
    // If the constructor ended at index 9, Branch {2} is Index 10.
    uint16_t stackIdx = 10;
    uint16_t nodeIdx = Values.Child(stackIdx);

    while (nodeIdx != INVALID_HEADER)
    {
        Result node = Values.Get(nodeIdx);
        if (node.Type == Types::Geometry2D)
        {
            RenderGeometry(nodeIdx, Length, Size, BaseTransform, Mirrored, Overlay);
        }
        else if (node.Type == Types::Texture2D)
        {
            RenderTexture(nodeIdx, Length, Size, BaseTransform, Mirrored, Overlay);
            memset((void *)Overlay, 0, Length * sizeof(Number));
        }
        nodeIdx = Values.Next(nodeIdx);
    }

    // Final Brightness Pass
    if (Bright < 255)
    {
        for (int32_t I = 0; I < Length * 3; I++)
            LEDs[I] = MultiplyBytePercentByte(LEDs[I], Bright);
    }

    return true;
}

void DisplayClass::RenderGeometry(uint16_t NodeIdx, int32_t Length, Vector2D DisplaySize,
                                  Coord2D Transform, bool Mirrored, Number *Overlay)
{
    Result typeRes = Values.Get(NodeIdx);
    if (!typeRes.Value)
        return;

    // Use fast child/next navigation instead of Find paths
    uint16_t opIdx = Values.Child(NodeIdx);
    uint16_t sizeIdx = Values.Next(opIdx);
    uint16_t fadeIdx = Values.Next(sizeIdx);
    uint16_t posIdx = Values.Next(fadeIdx);

    // 1. Get the Results first so we can check their internal Value pointers
    Result opRes = (opIdx != 0xFFFF) ? Values.Get(opIdx) : Result();
    Result sizeRes = (sizeIdx != 0xFFFF) ? Values.Get(sizeIdx) : Result();
    Result fadeRes = (fadeIdx != 0xFFFF) ? Values.Get(fadeIdx) : Result();
    Result posRes = (posIdx != 0xFFFF) ? Values.Get(posIdx) : Result();

    // 2. Extract types safely. If .Value is null (restoration in progress), use fallbacks.
    Geometries Type = *(Geometries *)typeRes.Value; // Already null-checked at start of function
    GeometryOperation Op = (opRes.Value) ? *(GeometryOperation *)opRes.Value : GeometryOperation::Add;
    Vector2D GSize = (sizeRes.Value) ? *(Vector2D *)sizeRes.Value : Vector2D(1, 1);
    Number GFade = (fadeRes.Value) ? *(Number *)fadeRes.Value : N(0.001);
    Coord2D GPos = (posRes.Value) ? *(Coord2D *)posRes.Value : Coord2D(0, 0, 0);

    if (GFade <= 0)
        GFade = N(0.001);
    Coord2D CurrentTransform = Transform.Join(GPos);

    int32_t GridW = DisplaySize.X.ToInt();
    int32_t GridH = DisplaySize.Y.ToInt();

    for (int32_t Y = 0; Y < GridH; Y++)
    {
        for (int32_t X = 0; X < GridW; X++)
        {
            // --- New Layout Resolution Switch ---
            int32_t GridIdx;
            uint32_t rowOffset = (GridH - Y - 1) * GridW;

            switch (Values.Get(0).Value ? *(Displays *)Values.Get(0).Value : Displays::Undefined) // Assuming DisplayType is stored in your class
            {
            case Displays::GenericLEDMatrixWeave:
                // Row (GridH - Y - 1) is what we check for odd/even to snake
                GridIdx = rowOffset + (((GridH - Y - 1) & 1) ? (GridW - 1 - X) : X);
                break;

            default:
                GridIdx = rowOffset + X;
                break;
            }
#if defined O_LAYOUT_USED
            int32_t PIdx = (Layout != nullptr) ? (int32_t)Layout[GridIdx] - 1 : GridIdx;
#else
            int32_t PIdx = GridIdx;
#endif

            if (PIdx < 0 || PIdx >= Length)
                continue;
            if (Op == GeometryOperation::Add && Overlay[PIdx] >= 1.0)
                continue;

            Vector2D P = CurrentTransform.TransformTo(Vector2D(X, Y));
            if (Mirrored)
                P = P.Mirror(Vector2D(0, 1));

            Number LocalAlpha = CalculateShapeAlpha(Type, P, GSize, GFade);
            if (LocalAlpha <= 0)
                continue;

            switch (Op)
            {
            case GeometryOperation::Add:
                Overlay[PIdx] = LimitZeroToOne(Overlay[PIdx] + LocalAlpha);
                break;
            case GeometryOperation::Cut:
                Overlay[PIdx] = LimitZeroToOne(Overlay[PIdx] - LocalAlpha);
                break;
            case GeometryOperation::Intersect:
                Overlay[PIdx] = LimitZeroToOne(Overlay[PIdx] * LocalAlpha);
                break;
            case GeometryOperation::XOR:
                Overlay[PIdx] = abs(Overlay[PIdx] - LocalAlpha);
                break;
            }
        }
    }
}

Number DisplayClass::CalculateShapeAlpha(Geometries Type, Vector2D P, Vector2D S, Number F)
{
    Number Distance = 0;

    switch (Type)
    {
    case Geometries::Box:
        Distance = min(S.X - abs(P.X), S.Y - abs(P.Y));
        break;

    case Geometries::Triangle:
        // Math: 2h|x|/w + y < h
        Distance = min(S.Y - P.Y - 2 * S.Y * abs(P.X) / S.X, P.Y);
        break;

    case Geometries::Elipse:
        // Fast Box Optimization: Skip calculation if outside bounding box + fade
        if (abs(P.X) > S.X + F || abs(P.Y) > S.Y + F)
            return 0;
        // Inner Fast Optimization: Full opacity if safely inside
        if (abs(P.X) < S.X * 0.7 - F && abs(P.Y) < S.Y * 0.7 - F)
            return 1;

        Distance = 1.0 - sqrt(sq(P.X) / sq(S.X) + sq(P.Y) / sq(S.Y));
        break;

    case Geometries::DoubleParabola:
        // Math: (abs(x) - w + y^2 * w / h^2)
        Distance = -(abs(P.X) - S.X + sq(P.Y) * S.X / sq(S.Y));
        break;

    case Geometries::HalfFill:
        return LimitZeroToOne(P.Y / F + 0.5);

    case Geometries::Fill:
        return N(1.0);

    default:
        return 0;
    }

    // Convert Distance to 0.0-1.0 alpha based on Fade
    return LimitZeroToOne(Distance / F + 0.5);
}

void DisplayClass::RenderTexture(uint16_t NodeIdx, int32_t Length, Vector2D DisplaySize,
                                 Coord2D Transform, bool Mirrored, Number *Overlay)
{
    Result texRes = Values.Get(NodeIdx);
    if (!texRes.Value)
        return;

    uint16_t colAIdx = Values.Child(NodeIdx);
    Result colARes = Values.Get(colAIdx);
    if (!colARes.Value)
        return;

    Textures2D TextureType = *(Textures2D *)texRes.Value;
    ColourClass ColourA = *(ColourClass *)colARes.Value;

    if (TextureType == Textures2D::Full)
    {
        for (int32_t I = 0; I < Length; I++)
        {
            if (Overlay[I] <= 0)
                continue;
            int32_t Offset = I * 3;
            ColourClass Pixel(LEDs[Offset + 1], LEDs[Offset], LEDs[Offset + 2], 255);
            Pixel.Layer(ColourA, Overlay[I]);
            LEDs[Offset] = Pixel.G;
            LEDs[Offset + 1] = Pixel.R;
            LEDs[Offset + 2] = Pixel.B;
        }
        return;
    }

    uint16_t colBIdx = Values.Next(colAIdx);
    uint16_t texPosIdx = Values.Next(colBIdx);
    uint16_t texWidIdx = Values.Next(texPosIdx);

    // 1. Fetch results
    Result resB = (colBIdx != 0xFFFF) ? Values.Get(colBIdx) : Result();
    Result resPos = (texPosIdx != 0xFFFF) ? Values.Get(texPosIdx) : Result();
    Result resWid = (texWidIdx != 0xFFFF) ? Values.Get(texWidIdx) : Result();

    // 2. Safe dereference with fallbacks
    // (Assuming black/transparent, zeroed coordinates, and 1.0 width as safe defaults)
    ColourClass ColourB = (resB.Value) ? *(ColourClass *)resB.Value : ColourClass(0, 0, 0, 0);
    Coord2D TexPos = (resPos.Value) ? *(Coord2D *)resPos.Value : Coord2D(0, 0, 0);
    Number TexWidth = (resWid.Value) ? *(Number *)resWid.Value : N(1.0);

    if (TexWidth <= 0)
        TexWidth = N(0.001);

    Coord2D CurrentTransform = Transform.Join(TexPos);
    int32_t GridW = DisplaySize.X.ToInt();
    int32_t GridH = DisplaySize.Y.ToInt();
    Number invTexWidthScaled = 1.0 / (TexWidth * 2.0);

    for (int32_t Y = 0; Y < GridH; Y++)
    {
        for (int32_t X = 0; X < GridW; X++)
        {
            // --- New Layout Resolution Switch ---
            int32_t GridIdx;
            uint32_t rowOffset = (GridH - Y - 1) * GridW;

            switch (Values.Get(0).Value ? *(Displays *)Values.Get(0).Value : Displays::Undefined) // Assuming DisplayType is stored in your class
            {
            case Displays::GenericLEDMatrixWeave:
                // Row (GridH - Y - 1) is what we check for odd/even to snake
                GridIdx = rowOffset + (((GridH - Y - 1) & 1) ? (GridW - 1 - X) : X);
                break;

            default:
                GridIdx = rowOffset + X;
                break;
            }
#if defined O_LAYOUT_USED
            int32_t PIdx = (Layout != nullptr) ? (int32_t)Layout[GridIdx] - 1 : GridIdx;
#else
            int32_t PIdx = GridIdx;
#endif

            if (PIdx < 0 || PIdx >= Length || Overlay[PIdx] <= 0)
                continue;

            Vector2D P = CurrentTransform.TransformTo(Vector2D(X, Y));
            if (Mirrored)
                P = P.Mirror(Vector2D(0, 1));

            ColourClass FinalColour = ColourB;
            if (TextureType == Textures2D::BlendLinear)
                FinalColour.Layer(ColourA, LimitZeroToOne((P.X * invTexWidthScaled) + 0.5));
            else if (TextureType == Textures2D::BlendCircular)
                FinalColour.Layer(ColourA, LimitZeroToOne((P.Length() * invTexWidthScaled) + 0.5));

            int32_t Offset = PIdx * 3;
            ColourClass Pixel(LEDs[Offset + 1], LEDs[Offset], LEDs[Offset + 2], 255);
            Pixel.Layer(FinalColour, Overlay[PIdx]);
            LEDs[Offset] = Pixel.G;
            LEDs[Offset + 1] = Pixel.R;
            LEDs[Offset + 2] = Pixel.B;
        }
    }
}