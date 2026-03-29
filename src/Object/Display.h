class DisplayClass : public BaseClass
{
private:
    Number *Overlay = nullptr;
    int32_t MaxLength = 0;
    void ManageOverlay(int32_t RequiredLength);

public:
    uint8_t *Layout = nullptr;
    PortNumber CurrentLink = -1; // Default empty reference
    uint8_t *LEDs = nullptr;

    DisplayClass(const Reference &ID, ObjectInfo Info = {Flags::None, 1});
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

    void RenderGeometry(uint8_t Index, int32_t Length, Vector2D DisplaySize,
                        Coord2D Transform, bool Mirrored, Number *Overlay);
    Number CalculateShapeAlpha(Geometries Type, Vector2D P, Vector2D S, Number F);
    void RenderTexture(uint8_t Index, int32_t Length, Vector2D DisplaySize,
                       Coord2D Transform, bool Mirrored, Number *Overlay);
};

DisplayClass::DisplayClass(const Reference &ID, ObjectInfo Info) : BaseClass(&Table, ID, Info)
{
    Type = ObjectTypes::Display;
    Name = "Display";

    // Branch {0}: Hardware Definition
    Displays initType = Displays::Undefined;
    Values.Set(&initType, sizeof(Displays), Types::Byte, {0});

    PortNumber initPort = -1;
    Values.Set(&initPort, sizeof(PortNumber), Types::PortNumber, {0, 0});

    int32_t initLen = 0;
    Values.Set(&initLen, sizeof(int32_t), Types::Integer, {0, 1});

    Vector2D initSize(0, 0);
    Values.Set(&initSize, sizeof(Vector2D), Types::Vector2D, {0, 2});

    Number initRatio = 1.0f;
    Values.Set(&initRatio, sizeof(Number), Types::Number, {0, 3});

    // Branch {1}: Control
    uint8_t initBright = 20;
    Values.Set(&initBright, sizeof(uint8_t), Types::Byte, {1});

    Coord2D initOffset(0, 0, 0);
    Values.Set(&initOffset, sizeof(Coord2D), Types::Coord2D, {1, 0});

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

    // Direct local find
    SearchResult linkRes = Values.Find({0, 0}, true);
    if (!linkRes.Value)
        return false;

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
            SearchResult res = Values.Find({0}, true);
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
                    Values.Set(&newLen, sizeof(int32_t), Types::Integer, {0, 1});
                    Values.Set(&newSize, sizeof(Vector2D), Types::Vector2D, {0, 2});
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
        SearchResult lenRes = Values.Find({0, 1}, true);
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

    // Allocate only if current buffer is too small or null
    if (RequiredLength > MaxLength || Overlay == nullptr)
    {
        // Allocate and zero-initialize to prevent garbage values
        Number *NewOverlay = new Number[RequiredLength]();
        if (!NewOverlay)
            return;

        if (Overlay != nullptr)
            delete[] Overlay;

        Overlay = NewOverlay;
        MaxLength = RequiredLength;
        ESP_LOGI("Display", "Overlay buffer allocated: %d elements", RequiredLength);
    }
}

bool DisplayClass::Run()
{
    // Fetch all control parameters once at start of frame
    SearchResult lenRes = Values.Find({0, 1}, true);
    SearchResult sizeRes = Values.Find({0, 2}, true);
    SearchResult brightRes = Values.Find({1});
    SearchResult offRes = Values.Find({1, 0});
    SearchResult mirRes = Values.Find({1, 1});

    if (!lenRes.Value || !sizeRes.Value || !brightRes.Value)
        return true;

    int32_t Length = *(int32_t *)lenRes.Value;
    Vector2D Size = *(Vector2D *)sizeRes.Value;
    uint8_t Bright = *(uint8_t *)brightRes.Value;
    Coord2D Offset = *(Coord2D *)offRes.Value;
    bool Mirrored = *(bool *)mirRes.Value;

    if (LEDs == nullptr || Overlay == nullptr || Length <= 0)
        return true;

    Coord2D BaseTransform = Coord2D(Size * 0.5 - Vector2D(0.5, 0.5), Vector2D(0)).Join(Offset);

    memset(LEDs, 0, Length * 3);
    memset((void *)Overlay, 0, Length * sizeof(Number));

    // Render Stack {2}
    for (uint8_t I = 0;; I++)
    {
        SearchResult node = Values.Find({2, I});
        if (node.Type == Types::Undefined || !node.Value || node.Length == 0)
            break;

        if (node.Type == Types::Geometry2D)
            RenderGeometry(I, Length, Size, BaseTransform, Mirrored, Overlay);
        else if (node.Type == Types::Texture2D)
        {
            RenderTexture(I, Length, Size, BaseTransform, Mirrored, Overlay);
            memset((void *)Overlay, 0, Length * sizeof(Number)); // Clear alpha for next texture layer
        }
    }

    // Final scaling
    if (Bright < 255)
    {
        for (int32_t I = 0; I < Length * 3; I++)
            LEDs[I] = MultiplyBytePercentByte(LEDs[I], Bright);
    }

    return true;
}

void DisplayClass::RenderGeometry(uint8_t Index, int32_t Length, Vector2D DisplaySize,
                                  Coord2D Transform, bool Mirrored, Number *Overlay)
{
    // 1. Parameter Extraction
    SearchResult typeRes = Values.Find({2, Index});
    SearchResult opRes   = Values.Find({2, Index, 0});
    SearchResult sizeRes = Values.Find({2, Index, 1});
    SearchResult fadeRes = Values.Find({2, Index, 2});
    SearchResult posRes  = Values.Find({2, Index, 3});

    // If the base node doesn't exist, exit immediately
    if (!typeRes.Value || typeRes.Type == Types::Undefined) return;

    // Use safe defaults if sub-parameters are missing
    Geometries Type      = *(Geometries*)typeRes.Value;
    GeometryOperation Op = opRes.Value ? *(GeometryOperation*)opRes.Value : GeometryOperation::Add;
    Vector2D GSize       = sizeRes.Value ? *(Vector2D*)sizeRes.Value : Vector2D(1, 1);
    Number GFade         = fadeRes.Value ? *(Number*)fadeRes.Value : Number(0.001f);
    Coord2D GPos         = posRes.Value ? *(Coord2D*)posRes.Value : Coord2D(0, 0, 0);

    if (GFade <= 0) GFade = 0.001f;
    Coord2D CurrentTransform = Transform.Join(GPos);

    int32_t GridW = (int32_t)DisplaySize.X;
    int32_t GridH = (int32_t)DisplaySize.Y;

    // 2. Grid Iteration
    for (int32_t Y = 0; Y < GridH; Y++)
    {
        for (int32_t X = 0; X < GridW; X++)
        {
            // Calculate index in the 2D grid
            int32_t GridIdx = (GridH - Y - 1) * GridW + X;
            int32_t PIdx = -1;

            // 3. Layout Mapping & Bounds Safety
            if (Layout != nullptr)
            {
                // Ensure we don't read past the end of the Layout mapping table
                // For Vysi 1.0, this table MUST be at least GridW * GridH (110) bytes
                if (GridIdx >= (GridW * GridH)) continue;

                uint8_t LayoutVal = Layout[GridIdx];
                
                // If Layout value is 0, there is no physical LED at this grid coordinate
                if (LayoutVal == 0) continue;
                
                PIdx = (int32_t)LayoutVal - 1;
            }
            else
            {
                PIdx = GridIdx;
            }

            // Final safety check: Is the calculated pixel index within the allocated Overlay/LED buffers?
            if (PIdx < 0 || PIdx >= Length) continue;

            // 4. Rendering Logic
            // Performance optimization: skip if adding to a pixel that's already full
            if (Op == GeometryOperation::Add && Overlay[PIdx] >= 1.0f) continue;

            Vector2D P = CurrentTransform.TransformTo(Vector2D(X, Y));
            if (Mirrored) P = P.Mirror(Vector2D(0, 1));

            Number LocalAlpha = CalculateShapeAlpha(Type, P, GSize, GFade);
            if (LocalAlpha <= 0) continue;

            // 5. Blending Operations
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
                    // Using standard XOR logic: |A - B|
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

void DisplayClass::RenderTexture(uint8_t Index, int32_t Length, Vector2D DisplaySize,
                                 Coord2D Transform, bool Mirrored, Number *Overlay)
{
    // 1. Safety Gate: Ensure buffers exist before any logic
    if (!LEDs || !Overlay || Length <= 0)
        return;

    // 2. Initial Type and Value Check
    SearchResult texRes = Values.Find({2, Index});
    if (texRes.Type != Types::Texture2D || !texRes.Value)
        return;

    Textures2D TextureType = *(Textures2D *)texRes.Value;

    // 3. Fetch primary attributes
    SearchResult colARes = Values.Find({2, Index, 0});
    if (!colARes.Value)
        return;
    ColourClass ColourA = *(ColourClass *)colARes.Value;

    // --- Fast Path: Full Fill ---
    if (TextureType == Textures2D::Full)
    {
        for (int32_t I = 0; I < Length; I++)
        {
            // Only process pixels with geometry alpha
            if (Overlay[I] <= 0)
                continue;

            int32_t Offset = I * 3;

            // Check bounds for the +2 offset to prevent heap corruption/exception
            if (Offset + 2 >= Length * 3)
                break;

            // Map current GRB from buffer to a temporary Pixel (R, G, B)
            // Note: LEDs[Offset] is G, LEDs[Offset+1] is R, LEDs[Offset+2] is B
            ColourClass Pixel(LEDs[Offset + 1], LEDs[Offset], LEDs[Offset + 2], 255);
            Pixel.Layer(ColourA, Overlay[I]);

            // Write back in GRB format
            LEDs[Offset] = Pixel.G;
            LEDs[Offset + 1] = Pixel.R;
            LEDs[Offset + 2] = Pixel.B;
        }
        return;
    }

    // --- Complex Path: Linear/Circular ---
    SearchResult colBRes = Values.Find({2, Index, 1});
    SearchResult texPosRes = Values.Find({2, Index, 2});
    SearchResult texWidRes = Values.Find({2, Index, 3});

    if (!colBRes.Value || !texPosRes.Value || !texWidRes.Value)
        return;

    ColourClass ColourB = *(ColourClass *)colBRes.Value;
    Coord2D TexPos = *(Coord2D *)texPosRes.Value;
    Number TexWidth = *(Number *)texWidRes.Value;

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

            // Layout Safety: Ensure GridIdx doesn't exceed the layout array itself
            // If the grid is 11x10, Layout must be at least 110 bytes.
            int32_t PIdx = (Layout != nullptr) ? (int32_t)Layout[GridIdx] - 1 : GridIdx;

            // CRITICAL: PIdx bounds check MUST happen before accessing Overlay[PIdx]
            if (PIdx < 0 || PIdx >= Length || Overlay[PIdx] <= 0)
                continue;

            Vector2D P = CurrentTransform.TransformTo(Vector2D(X, Y));
            if (Mirrored)
                P = P.Mirror(Vector2D(0, 1));

            ColourClass FinalColour = ColourB;
            Number Distance = 0;

            if (TextureType == Textures2D::BlendLinear)
            {
                Distance = LimitZeroToOne((P.X * invTexWidthScaled) + 0.5f);
                FinalColour.Layer(ColourA, Distance);
            }
            else if (TextureType == Textures2D::BlendCircular)
            {
                Distance = LimitZeroToOne((P.Length() * invTexWidthScaled) + 0.5f);
                FinalColour.Layer(ColourA, Distance);
            }

            int32_t Offset = PIdx * 3;
            if (Offset + 2 >= Length * 3)
                continue;

            // Consistent GRB Mapping
            ColourClass Pixel(LEDs[Offset + 1], LEDs[Offset], LEDs[Offset + 2], 255);
            Pixel.Layer(FinalColour, Overlay[PIdx]);

            LEDs[Offset] = Pixel.G;
            LEDs[Offset + 1] = Pixel.R;
            LEDs[Offset + 2] = Pixel.B;
        }
    }
}