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

DisplayClass::DisplayClass(const Reference &ID, FlagClass Flags, RunInfo Info) : BaseClass(&Table, ID, Flags, Info)
{
    Type = ObjectTypes::Display;
    Name = "Display";

    // Branch {0}: Hardware Definition
    Displays initType = Displays::Undefined;
    Values.Set(&initType, sizeof(Displays), Types::Byte, Reference({0}));

    PortNumber initPort = -1;
    Values.Set(&initPort, sizeof(PortNumber), Types::PortNumber, Reference({0, 0}));

    int32_t initLen = 0;
    Values.Set(&initLen, sizeof(int32_t), Types::Integer, Reference({0, 1}));

    Vector2D initSize(0, 0);
    Values.Set(&initSize, sizeof(Vector2D), Types::Vector2D, Reference({0, 2}));

    Number initRatio = 1.0f;
    Values.Set(&initRatio, sizeof(Number), Types::Number, Reference({0, 3}));

    // Branch {1}: Control
    uint8_t initBright = 20;
    Values.Set(&initBright, sizeof(uint8_t), Types::Byte, Reference({1}));

    Coord2D initOffset(0, 0, 0);
    Values.Set(&initOffset, sizeof(Coord2D), Types::Coord2D, Reference({1, 0}));

    bool initFalse = false;
    Values.Set(&initFalse, sizeof(bool), Types::Bool, {1, 1}); // Mirror
    Values.Set(&initFalse, sizeof(bool), Types::Bool, {1, 2}); // WrapX
    Values.Set(&initFalse, sizeof(bool), Types::Bool, {1, 3}); // WrapY
};

DisplayClass::~DisplayClass()
{
    Disconnect();
    if (Overlay)
        delete[] Overlay;
};

bool DisplayClass::Connect(BaseClass *Object, int32_t Index)
{
    if (Object == nullptr)
        Object = this;

    Bookmark linkMark = Values.Find({0, 0}, true);
    if (linkMark.Index == 0xFFFF)
        return false;

    Result linkRes = Values.Get(linkMark);

    // 1. Try Daisy-chain (Reference)
    if (linkRes.Type == Types::Reference)
    {
        Reference Link = *(Reference *)linkRes.Value;
        if (Link.IsValid())
        {
            BaseClass *Target = Objects.At(Link);
            if (Target && Target->Type == ObjectTypes::Display)
                return static_cast<DisplayClass *>(Target)->Connect(Object, Index + 1);
        }
        return false;
    }

    // 2. Try Direct Board Connection (PortNumber)
    PortNumber Port = *(PortNumber *)linkRes.Value;
    if (Port.IsValid())
    {
        if (Board.ConnectLED(Object, Port, Index + 1))
        {
            if (Object->Type == ObjectTypes::Display)
                static_cast<DisplayClass *>(Object)->CurrentLink = Port;
            return true;
        }
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
    bool HardwareChanged = (Index.PathLen() == 0);

    if (!HardwareChanged && Index.PathLen() > 0 && Index.Path[0] == 0)
    {
        if (Index.PathLen() == 1) // Displays::Type changed
        {
            Bookmark resMark = Values.Find({0}, true);
            Result res = Values.Get(resMark);
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
                    Values.Set(&newLen, sizeof(int32_t), Types::Integer, Reference({0, 1}));
                    Values.Set(&newSize, sizeof(Vector2D), Types::Vector2D, Reference({0, 2}));
                    HardwareChanged = true;
                }
            }
        }
        else if (Index.PathLen() > 1 && (Index.Path[1] == 0 || Index.Path[1] == 1))
        {
            HardwareChanged = true;
        }
    }

    if (HardwareChanged)
    {
        Bookmark lenMark = Values.Find({0, 1}, true);
        Result lenRes = Values.Get(lenMark);
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
    // Step 1: Bookmark parameters
    Bookmark lenMark = Values.Find({0, 1}, true);
    Bookmark sizeMark = Values.Find({0, 2}, true);
    Bookmark brightMark = Values.Find({1}, true);
    Bookmark offMark = Values.Find({1, 0}, true);
    Bookmark mirMark = Values.Find({1, 1}, true);

    Result lenRes = Values.Get(lenMark);
    Result sizeRes = Values.Get(sizeMark);
    Result brightRes = Values.Get(brightMark);

    if (!lenRes.Value || !sizeRes.Value || !brightRes.Value)
        return true;

    int32_t Length = *(int32_t *)lenRes.Value;
    Vector2D Size = *(Vector2D *)sizeRes.Value;
    uint8_t Bright = *(uint8_t *)brightRes.Value;
    Coord2D Offset = *(Coord2D *)Values.Get(offMark).Value;
    bool Mirrored = *(bool *)Values.Get(mirMark).Value;

    if (LEDs == nullptr || Overlay == nullptr || Length <= 0)
        return true;

    Coord2D BaseTransform = Coord2D(Size * 0.5 - Vector2D(0.5, 0.5), Vector2D(0)).Join(Offset);

    memset(LEDs, 0, Length * 3);
    memset((void *)Overlay, 0, Length * sizeof(Number));

    // Step 2: Iterate Render Stack {2}
    Bookmark stackRoot = Values.Find({2}, true);
    if (stackRoot.Index != 0xFFFF)
    {
        uint16_t nodeIdx = Values.Child(stackRoot.Index);
        while (nodeIdx != 0xFFFF)
        {
            Result node = Values.Get(nodeIdx);
            if (node.Type == Types::Geometry2D)
            {
                RenderGeometry(nodeIdx, Length, Size, BaseTransform, Mirrored, Overlay);
            }
            else if (node.Type == Types::Texture2D)
            {
                RenderTexture(nodeIdx, Length, Size, BaseTransform, Mirrored, Overlay);
                memset((void *)Overlay, 0, Length * sizeof(Number)); // Clear alpha for next texture
            }
            nodeIdx = Values.Next(nodeIdx);
        }
    }

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