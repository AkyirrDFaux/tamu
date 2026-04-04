class DisplayClass : public BaseClass
{
private:
    Number *Overlay = nullptr;
    int32_t MaxLength = 0;
    void ManageOverlay(int32_t RequiredLength);

public:
    uint8_t *Layout = nullptr;
    PortNumber CurrentLink = -1;
    uint8_t *LEDs = nullptr;

    DisplayClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});
    ~DisplayClass();

    void Setup(const Reference &Index);
    bool Run();

    bool Connect(BaseClass *Object = nullptr, int32_t Index = -1);
    bool Disconnect();

    static void SetupBridge(BaseClass *Base, const Reference &Index)
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

    // Values starts empty. We build it linearly to avoid path parsing.

    // --- Branch {0}: Hardware Definition ---
    Displays initType = Displays::Undefined;
    Values.Set(&initType, sizeof(Displays), Types::Display, 0); // {0}

    PortNumber initPort = -1;
    uint16_t b0 = Values.InsertChild(&initPort, sizeof(PortNumber), Types::PortNumber, 0); // {0, 0}

    int32_t initLen = 0;
    uint16_t b0_1 = Values.InsertNext(&initLen, sizeof(int32_t), Types::Integer, b0); // {0, 1}

    Vector2D initSize(0, 0);
    uint16_t b0_2 = Values.InsertNext(&initSize, sizeof(Vector2D), Types::Vector2D, b0_1); // {0, 2}

    Number initRatio = 1.0f;
    Values.InsertNext(&initRatio, sizeof(Number), Types::Number, b0_2); // {0, 3}

    // --- Branch {1}: Control ---
    uint8_t initBright = 20;
    // InsertNext after the end of Branch {0}. Skip logic handles finding the end.
    uint16_t b1 = Values.InsertNext(&initBright, sizeof(uint8_t), Types::Byte, 0); // {1}

    Coord2D initOffset(0, 0, 0);
    uint16_t b1_0 = Values.InsertChild(&initOffset, sizeof(Coord2D), Types::Coord2D, b1); // {1, 0}

    bool initFalse = false;
    uint16_t b1_1 = Values.InsertNext(&initFalse, sizeof(bool), Types::Bool, b1_0); // {1, 1}
    uint16_t b1_2 = Values.InsertNext(&initFalse, sizeof(bool), Types::Bool, b1_1); // {1, 2}
    Values.InsertNext(&initFalse, sizeof(bool), Types::Bool, b1_2);                 // {1, 3}
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

void DisplayClass::Setup(const Reference &Index)
{
    // PathLen is a function call; storing it once saves bytes
    uint8_t len = Index.PathLen();
    bool HardwareChanged = (len == 0);

    // Hardcoded indices from the linear constructor:
    // {0}   = Index 0 (Displays Type)
    // {0, 0} = Index 1 (Port)
    // {0, 1} = Index 2 (Length)
    // {0, 2} = Index 3 (Size)

    if (!HardwareChanged && len > 0 && Index.Path[0] == 0)
    {
        if (len == 1) // Displays::Type changed ({0})
        {
            Result res = Values.Get(0);
            if (res.Value)
            {
                Displays Type = *(Displays *)res.Value;
                int32_t newLen = 0;
                Vector2D newSize(0, 0);

                if (Type == Displays::GenericLEDMatrix)
                {
                    newLen = 256;
                    newSize = Vector2D(16, 16);
                    Layout = nullptr;
                }
                else if (Type == Displays::Vysi_v1_0)
                {
                    newLen = 86;
                    newSize = Vector2D(11, 10);
                    Layout = LayoutVysiv1_0;
                }

                if (newLen > 0)
                {
                    Values.Set(&newLen, sizeof(int32_t), Types::Integer, 2);    // {0, 1}
                    Values.Set(&newSize, sizeof(Vector2D), Types::Vector2D, 3); // {0, 2}
                    HardwareChanged = true;
                }
            }
        }
        // Check if Port {0,0} or Length {0,1} changed
        else if (len > 1 && (Index.Path[1] == 0 || Index.Path[1] == 1))
        {
            HardwareChanged = true;
        }
    }

    if (HardwareChanged)
    {
        Result lenRes = Values.Get(2); // {0, 1}
        if (lenRes.Value)
            ManageOverlay(*(int32_t *)lenRes.Value);

        Disconnect();
        Connect();
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
    Coord2D BaseTransform = Coord2D(Size * 0.5 - Vector2D(0.5, 0.5), Vector2D(0)).Join(Offset);

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

    Geometries Type = *(Geometries *)typeRes.Value;
    GeometryOperation Op = (opIdx != 0xFFFF) ? *(GeometryOperation *)Values.Get(opIdx).Value : GeometryOperation::Add;
    Vector2D GSize = (sizeIdx != 0xFFFF) ? *(Vector2D *)Values.Get(sizeIdx).Value : Vector2D(1, 1);
    Number GFade = (fadeIdx != 0xFFFF) ? *(Number *)Values.Get(fadeIdx).Value : Number(0.001f);
    Coord2D GPos = (posIdx != 0xFFFF) ? *(Coord2D *)Values.Get(posIdx).Value : Coord2D(0, 0, 0);

    if (GFade <= 0)
        GFade = 0.001f;
    Coord2D CurrentTransform = Transform.Join(GPos);

    int32_t GridW = (int32_t)DisplaySize.X;
    int32_t GridH = (int32_t)DisplaySize.Y;

    for (int32_t Y = 0; Y < GridH; Y++)
    {
        for (int32_t X = 0; X < GridW; X++)
        {
            int32_t GridIdx = (GridH - Y - 1) * GridW + X;
            int32_t PIdx = (Layout != nullptr) ? (int32_t)Layout[GridIdx] - 1 : GridIdx;

            if (PIdx < 0 || PIdx >= Length)
                continue;
            if (Op == GeometryOperation::Add && Overlay[PIdx] >= 1.0f)
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
        return 1.0;

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

    ColourClass ColourB = *(ColourClass *)Values.Get(colBIdx).Value;
    Coord2D TexPos = *(Coord2D *)Values.Get(texPosIdx).Value;
    Number TexWidth = *(Number *)Values.Get(texWidIdx).Value;

    if (TexWidth <= 0)
        TexWidth = 0.001f;

    Coord2D CurrentTransform = Transform.Join(TexPos);
    int32_t GridW = (int32_t)DisplaySize.X;
    int32_t GridH = (int32_t)DisplaySize.Y;
    Number invTexWidthScaled = 1.0f / (TexWidth * 2.0f);

    for (int32_t Y = 0; Y < GridH; Y++)
    {
        for (int32_t X = 0; X < GridW; X++)
        {
            int32_t GridIdx = (GridH - Y - 1) * GridW + X;
            int32_t PIdx = (Layout != nullptr) ? (int32_t)Layout[GridIdx] - 1 : GridIdx;

            if (PIdx < 0 || PIdx >= Length || Overlay[PIdx] <= 0)
                continue;

            Vector2D P = CurrentTransform.TransformTo(Vector2D(X, Y));
            if (Mirrored)
                P = P.Mirror(Vector2D(0, 1));

            ColourClass FinalColour = ColourB;
            if (TextureType == Textures2D::BlendLinear)
                FinalColour.Layer(ColourA, LimitZeroToOne((P.X * invTexWidthScaled) + 0.5f));
            else if (TextureType == Textures2D::BlendCircular)
                FinalColour.Layer(ColourA, LimitZeroToOne((P.Length() * invTexWidthScaled) + 0.5f));

            int32_t Offset = PIdx * 3;
            ColourClass Pixel(LEDs[Offset + 1], LEDs[Offset], LEDs[Offset + 2], 255);
            Pixel.Layer(FinalColour, Overlay[PIdx]);
            LEDs[Offset] = Pixel.G;
            LEDs[Offset + 1] = Pixel.R;
            LEDs[Offset + 2] = Pixel.B;
        }
    }
}