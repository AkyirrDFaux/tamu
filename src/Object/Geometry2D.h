class Geometry2DClass : public BaseClass
{
public:
    enum Value
    {
        Geometry,
        Operation,
        Fade,
        Position,
        Size
    };

    Geometry2DClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup(int32_t Index = -1);

    static void SetupBridge(BaseClass *Base, int32_t Index) { static_cast<Geometry2DClass *>(Base)->Setup(Index); }
    static constexpr VTable Table = {
        .Setup = Geometry2DClass::SetupBridge,
        .Run = BaseClass::DefaultRun,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};

    void Render(int32_t Length, Vector2D DisplaySize, Number Ratio, Coord2D Transform, bool Mirrored, uint8_t *Layout, Number *Overlay);
};

constexpr VTable Geometry2DClass::Table;

Geometry2DClass::Geometry2DClass(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags)
{
    Type = ObjectTypes::Geometry2D;
    Name = "Geometry";

    ValueSet(Geometries::None);
    ValueSet(GeometryOperation::Add);
};

void Geometry2DClass::Setup(int32_t Index)
{
    if (Index != -1 && Index != 0)
        return;

    if (Values.Type(Geometry) != Types::Geometry2D)
        return;

    switch (ValueGet<Geometries>(Geometry))
    {
    case Geometries::Box:
    case Geometries::Elipse:
    case Geometries::DoubleParabola:
    case Geometries::Triangle:
        if (Values.Type(Size) != Types::Vector2D)
            ValueSet(Vector2D(1, 1), Size);
    case Geometries::HalfFill:
        if (Values.Type(Fade) != Types::Number)
            ValueSet<Number>(1, Fade);
        if (Values.Type(Position) != Types::Coord2D)
            ValueSet(Coord2D(), Position);
        break;
    default:
        break;
    }
};

void Geometry2DClass::Render(int32_t Length, Vector2D DisplaySize, Number Ratio, Coord2D Transform, bool Mirrored, uint8_t *Layout, Number *Overlay)
{
    if (Values.Type(Geometry) != Types::Geometry2D || Values.Type(Operation) != Types::GeometryOperation)
    {
        ReportError(Status::MissingModule);
        return;
    }
    Geometries Type = ValueGet<Geometries>(Geometry);
    GeometryOperation Operation = ValueGet<GeometryOperation>(Value::Operation);

    Number Fade;
    Coord2D Position;
    Vector2D Size;

    switch (Type)
    {
    case Geometries::Box:
    case Geometries::Elipse:
    case Geometries::DoubleParabola:
    case Geometries::Triangle:
        if (Values.Type(Value::Size) != Types::Vector2D)
            return;
        Size = ValueGet<Vector2D>(Value::Size);
    case Geometries::HalfFill:
        if (Values.Type(Value::Fade) != Types::Number || Values.Type(Value::Position) != Types::Coord2D)
            return;
        Fade = ValueGet<Number>(Value::Fade);
        Position = ValueGet<Coord2D>(Value::Position);
        break;
    default:
        break;
    }

    Transform = Transform.Join(Position);
    for (int32_t Y = 0; Y < int32_t(DisplaySize.Y); Y++)
    {
        for (int32_t X = 0; X < int32_t(DisplaySize.X); X++)
        {
            int32_t Index = (DisplaySize.Y - Y - 1) * DisplaySize.X + X; // Invert Y due to layout coords |''
            if (Layout != nullptr)
            {
                if (Layout[Index] == 0)
                    continue;
                Index = Layout[Index] - 1;
            }

            if (Operation == GeometryOperation::Add && Overlay[Index] >= Number(1))
                continue;
            else if ((Operation == GeometryOperation::Cut || Operation == GeometryOperation::Intersect) && Overlay[Index] <= Number(0))
                continue;

            Vector2D Centered = Transform.TransformTo(Vector2D(X, Y));
            if (Mirrored)
                Centered = Centered.Mirror(Vector2D(0, 1));

            // Calculate shape
            Number LocalOverlay = 0;

            switch (Type)
            {
            case Geometries::Box:
                LocalOverlay = min(Size.X - abs(Centered.X), Size.Y - abs(Centered.Y)) / Fade + 0.5;
                break;
            case Geometries::Triangle:
                // 2h|x|/w + y < h
                LocalOverlay = min(Size.Y - Centered.Y - 2 * Size.Y * abs(Centered.X) / Size.X, Centered.Y) / Fade + 0.5;
                break;
            case Geometries::Elipse:
                // Size is radius
                if (abs(Centered.X) > Size.X + Fade || abs(Centered.Y) > Size.Y + Fade) // Fast box, because sqrt is slow
                    LocalOverlay = 0;
                else if (abs(Centered.X) < Size.X * 0.7 - Fade && abs(Centered.Y) < Size.Y * 0.7 - Fade) // Fast inner rectangle (of side size 2*r*sqrt(2))
                    LocalOverlay = 1;
                else
                    LocalOverlay = (1 - sqrt(sq(Centered.X) / sq(Size.X) + sq(Centered.Y) / sq(Size.Y))) / Fade + 0.5;
                break;
            case Geometries::DoubleParabola:
                LocalOverlay = -(abs(Centered.X) - Size.X + sq(Centered.Y) * Size.X / sq(Size.Y)) / Fade + 0.5;
                break;
            case Geometries::HalfFill:
                LocalOverlay = Centered.Y / Fade + 0.5;
                break;
            case Geometries::Fill:
                LocalOverlay = 1;
                break;
            default:
                ReportError(Status::InvalidValue);
                break;
            }

            LocalOverlay = LimitZeroToOne(LocalOverlay);

            if (Operation == GeometryOperation::Add)
                Overlay[Index] = Overlay[Index] + LocalOverlay;
            else if (Operation == GeometryOperation::Cut)
                Overlay[Index] = Overlay[Index] - LocalOverlay;
            else if (Operation == GeometryOperation::Intersect)
                Overlay[Index] = Overlay[Index] * LocalOverlay;
            else if (Operation == GeometryOperation::XOR)
                Overlay[Index] = max(Overlay[Index], LocalOverlay) - min(Overlay[Index], LocalOverlay);

            Overlay[Index] = LimitZeroToOne(Overlay[Index]);
        }
    }
}

/*
Number DoubleParabola(CoordClass Current, CoordClass Center,
                     Number Width, Number Height, Number Fade)
{
    Number Distance = (abs(Current.X - Center.X) - Width + Square(Current.Y - Center.Y) / (Square(Height) / Width)) / Fade;

    return 1 - LimitZeroToOne(Distance);
}
*/