class DisplayClass : public BaseClass
{
private:
    Number *Overlay = nullptr; 
    int32_t MaxLength = 0;     
    void ManageOverlay(int32_t RequiredLength);

public:
    uint8_t *Layout = nullptr; 
    Reference CurrentLink = Reference(); // Default empty reference
    uint8_t *LEDs = nullptr;             

    DisplayClass(const Reference &ID, ObjectInfo Info = {Flags::None, 1});
    ~DisplayClass();

    void Setup(const Reference &Index);
    bool Run();

    bool Connect(BaseClass *Object = nullptr, int32_t Index = -1);
    bool Disconnect(BaseClass *Object = nullptr);

    static void SetupBridge(BaseClass *Base, const Reference &Index) { 
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
    BaseClass::Type = ObjectTypes::Display;
    Name = "Display";

    // Initialize local paths using the implicit list constructor
    Values.Set(Displays::Undefined, {0});
    Values.Set<Reference>(Reference(), {0, 0}); // Connection Link (Port/Object)
    Values.Set<int32_t>(0, {0, 1});             // Length
    Values.Set(Vector2D(0, 0), {0, 2});         // Size
    Values.Set<Number>(1, {0, 3});              // Ratio
    Values.Set<uint8_t>(20, {0, 4});            // Brightness
    Values.Set(Coord2D(0, 0, 0), {0, 5});       // Offset
    Values.Set(false, {0, 6});                  // Mirroring
    Values.Set(false, {0, 7});                  // Wrapping X
    Values.Set(false, {0, 8});                  // Wrapping Y
};

DisplayClass::~DisplayClass()
{
    Disconnect();
    if (Overlay) delete[] Overlay;
};

bool DisplayClass::Connect(BaseClass *Object, int32_t Index)
{
    if (Object == nullptr) Object = this;

    Getter<Reference> Link = Values.Get<Reference>({0, 0});
    if (!Link.Success || Link.Value == Reference())
        return false;

    BaseClass *Target = Objects.At(Link.Value);
    if (!Target) return false;

    this->CurrentLink = Link.Value;

    if (Target->Type == ObjectTypes::Board)
    {
        // Reference stores path from Path[0] onwards. 
        // Typically {1, PortIndex} for Board connections.
        if (Link.Value.PathLen() < 2) return false;

        uint8_t Port = Link.Value.Path[1]; 
        return static_cast<BoardClass *>(Target)->Connect(Object, Port, Index + 1);
    }
    else
    {
        return static_cast<DisplayClass *>(Target)->Connect(Object, Index + 1);
    }
}

bool DisplayClass::Disconnect(BaseClass *Object)
{
    if (Object == nullptr) Object = this;
    if (CurrentLink == Reference()) return false;

    BaseClass *Target = Objects.At(CurrentLink);
    if (!Target) return false;

    bool Success = false;
    if (Target->Type == ObjectTypes::Board)
    {
        if (CurrentLink.PathLen() < 2) return false;

        uint8_t Port = CurrentLink.Path[1];
        Success = static_cast<BoardClass *>(Target)->Disconnect(Object, Port);
    }
    else
    {
        Success = static_cast<DisplayClass *>(Target)->Disconnect(Object);
    }

    if (Object == this)
    {
        this->LEDs = nullptr;
        this->CurrentLink = Reference();
    }
    return Success;
}

void DisplayClass::Setup(const Reference &Index)
{
    bool HardwareChanged = (Index.PathLen() == 0);

    // Checking specifically for hardware branch {0, ...}
    if (!HardwareChanged && Index.PathLen() > 0 && Index.Path[0] == 0)
    {
        if (Index.PathLen() == 1) // Displays::Type changed
        {
            Displays Type = Values.Get<Displays>({0}).Value;
            switch (Type)
            {
            case Displays::GenericLEDMatrix:
                Values.Set<int32_t>(256, {0, 1});
                Values.Set(Vector2D(16, 16), {0, 2});
                HardwareChanged = true;
                break;
            case Displays::Vysi_v1_0:
                Values.Set<int32_t>(86, {0, 1});
                Values.Set(Vector2D(11, 10), {0, 2});
                Layout = LayoutVysiv1_0;
                HardwareChanged = true;
                break;
            default: break;
            }
        }
        else if (Index.PathLen() > 1 && (Index.Path[1] == 0 || Index.Path[1] == 1))
        {
            HardwareChanged = true;
        }
    }

    if (HardwareChanged)
    {
        Getter<int32_t> Length = Values.Get<int32_t>({0, 1});
        if (Length.Success) ManageOverlay(Length.Value);
        Disconnect();
        Connect();
    }
}

void DisplayClass::ManageOverlay(int32_t RequiredLength)
{
    if (RequiredLength <= 0) return;

    // Allocate only if current buffer is too small or null
    if (RequiredLength > MaxLength || Overlay == nullptr)
    {
        // Allocate and zero-initialize to prevent garbage values
        Number* NewOverlay = new Number[RequiredLength]();
        if (!NewOverlay) return; 

        if (Overlay != nullptr) delete[] Overlay;

        Overlay = NewOverlay;
        MaxLength = RequiredLength;
        ESP_LOGI("Display", "Overlay buffer allocated: %d elements", RequiredLength);
    }
}

bool DisplayClass::Run()
{
    // Access values using bit-tagged local paths
    Getter<int32_t> Length = Values.Get<int32_t>({0, 1});
    Getter<Vector2D> Size = Values.Get<Vector2D>({0, 2});
    Getter<uint8_t> Brightness = Values.Get<uint8_t>({0, 4});
    Getter<Coord2D> Offset = Values.Get<Coord2D>({0, 5});
    Getter<bool> Mirrored = Values.Get<bool>({0, 6});

    if (!Length.Success || !Size.Success || !Brightness.Success || !Offset.Success)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    if (LEDs == nullptr || Overlay == nullptr || Length.Value <= 0 || Length.Value > MaxLength)
    {
        ReportError(Status::PortError);
        return true;
    }

    Coord2D BaseTransform = Coord2D(Size.Value * 0.5 - Vector2D(0.5, 0.5), Vector2D(0)).Join(Offset.Value);

    memset(LEDs, 0, Length.Value * 3);
    memset((void *)Overlay, 0, Length.Value * sizeof(Number));

    for (uint8_t I = 0;; I++)
    {
        Types NodeType = Values.Type({1, I});
        if (NodeType == Types::Undefined) break;

        if (NodeType == Types::Geometry2D)
        {
            RenderGeometry(I, Length.Value, Size.Value, BaseTransform, Mirrored.Value, Overlay);
        }
        else if (NodeType == Types::Texture2D)
        {
            RenderTexture(I, Length.Value, Size.Value, BaseTransform, Mirrored.Value, Overlay);
            memset((void *)Overlay, 0, Length.Value * sizeof(Number));
        }
    }

    // Global brightness scaling
    if (Brightness.Value < 255)
    {
        int32_t TotalBytes = Length.Value * 3;
        for (int32_t I = 0; I < TotalBytes; I++)
        {
            LEDs[I] = MultiplyBytePercentByte(LEDs[I], Brightness.Value);
        }
    }

    return true;
}

void DisplayClass::RenderGeometry(uint8_t Index, int32_t Length, Vector2D DisplaySize,
                                  Coord2D Transform, bool Mirrored, Number *Overlay)
{
    Geometries Type = Values.Get<Geometries>({1, Index}).Value;
    GeometryOperation Op = Values.Get<GeometryOperation>({1, Index, 0}).Value;
    Vector2D GSize = Values.Get<Vector2D>({1, Index, 1}).Value;
    Number GFade = Values.Get<Number>({1, Index, 2}).Value;
    Coord2D GPos = Values.Get<Coord2D>({1, Index, 3}).Value;

    if (GFade <= 0) GFade = 0.001;
    Coord2D CurrentTransform = Transform.Join(GPos);

    int32_t GridW = int32_t(DisplaySize.X);
    int32_t GridH = int32_t(DisplaySize.Y);

    for (int32_t Y = 0; Y < GridH; Y++)
    {
        for (int32_t X = 0; X < GridW; X++)
        {
            int32_t GridIdx = (GridH - Y - 1) * GridW + X;
            int32_t PIdx = GridIdx;

            // 1. Safety check for Layout indexing
            if (Layout != nullptr)
            {
                if (Layout[GridIdx] == 0) continue;
                PIdx = Layout[GridIdx] - 1;
            }

            // 2. Safety check for Overlay/LED buffer indexing
            if (PIdx < 0 || PIdx >= Length) continue;

            if (Op == GeometryOperation::Add && Overlay[PIdx] >= 1.0) continue;
            if ((Op == GeometryOperation::Cut || Op == GeometryOperation::Intersect) && Overlay[PIdx] <= 0) continue;

            Vector2D P = CurrentTransform.TransformTo(Vector2D(X, Y));
            if (Mirrored) P = P.Mirror(Vector2D(0, 1));

            Number LocalAlpha = CalculateShapeAlpha(Type, P, GSize, GFade);

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
                Overlay[PIdx] = LimitZeroToOne(max(Overlay[PIdx], LocalAlpha) - min(Overlay[PIdx], LocalAlpha));
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
    if (Values.Type({1, Index}) != Types::Texture2D) return;

    Textures2D TextureType = Values.Get<Textures2D>({1, Index}).Value;
    ColourClass ColourA = Values.Get<ColourClass>({1, Index, 0}).Value;

    if (TextureType == Textures2D::Full)
    {
        for (int32_t I = 0; I < Length; I++)
        {
            if (Overlay[I] <= 0) continue;
            int32_t Offset = I * 3;

            ColourClass Pixel(LEDs[Offset + 1], LEDs[Offset], LEDs[Offset + 2], 255); //GRB
            Pixel.Layer(ColourA, Overlay[I]);

            LEDs[Offset] = Pixel.G;
            LEDs[Offset + 1] = Pixel.R;
            LEDs[Offset + 2] = Pixel.B;
        }
        return;
    }

    ColourClass ColourB = Values.Get<ColourClass>({1, Index, 1}).Value;
    Coord2D TexPos = Values.Get<Coord2D>({1, Index, 2}).Value;
    Number TexWidth = Values.Get<Number>({1, Index, 3}).Value;
    if (TexWidth <= 0) TexWidth = 0.001;

    Coord2D CurrentTransform = Transform.Join(TexPos);
    int32_t GridW = int32_t(DisplaySize.X);
    int32_t GridH = int32_t(DisplaySize.Y);

    for (int32_t Y = 0; Y < GridH; Y++)
    {
        for (int32_t X = 0; X < GridW; X++)
        {
            int32_t GridIdx = (GridH - Y - 1) * GridW + X;
            int32_t PIdx = GridIdx;

            if (Layout != nullptr)
            {
                if (Layout[GridIdx] == 0) continue;
                PIdx = Layout[GridIdx] - 1;
            }

            if (PIdx < 0 || PIdx >= Length || Overlay[PIdx] <= 0) continue;

            Vector2D P = CurrentTransform.TransformTo(Vector2D(X, Y));
            if (Mirrored) P = P.Mirror(Vector2D(0, 1));

            ColourClass FinalColour;
            Number Distance = 0;

            if (TextureType == Textures2D::BlendLinear)
            {
                Distance = LimitZeroToOne((P.X / TexWidth / 2.0) + 0.5);
                FinalColour = ColourB;
                FinalColour.Layer(ColourA, Distance);
            }
            else if (TextureType == Textures2D::BlendCircular)
            {
                Distance = LimitZeroToOne((P.Length() / TexWidth / 2.0) + 0.5);
                FinalColour = ColourB;
                FinalColour.Layer(ColourA, Distance);
            }

            int32_t Offset = PIdx * 3;

            ColourClass Pixel(LEDs[Offset], LEDs[Offset + 1], LEDs[Offset + 2], 255);
            Pixel.Layer(FinalColour, Overlay[PIdx]);

            LEDs[Offset] = Pixel.G;
            LEDs[Offset + 1] = Pixel.R;
            LEDs[Offset + 2] = Pixel.B;
        }
    }
}