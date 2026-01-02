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

    void Render(int32_t Length, Vector2D DisplaySize, Number Ratio, Coord2D Transform, bool Mirrored, byte *Layout, Number *Overlay);
};

Geometry2DClass::Geometry2DClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    Type = ObjectTypes::Geometry2D;
    Name = "Geometry";

    Values.Add(Geometries::None);
    Values.Add(GeometryOperation::Add);
};

void Geometry2DClass::Setup(int32_t Index)
{
    if (Index != -1 && Index != 0)
        return;

    Geometries *Type = Values.At<Geometries>(Geometry);

    switch (*Type)
    {
    case Geometries::Box:
    case Geometries::Elipse:
    case Geometries::DoubleParabola:
    case Geometries::Triangle:
        if (!Values.IsValid(Size))
            Values.Add(Vector2D(1, 1), Size);
    case Geometries::HalfFill:
        if (!Values.IsValid(Fade))
            Values.Add<Number>(1, Fade);
        if (!Values.IsValid(Position))
            Values.Add(Coord2D(), Position);
        break;
    }
};

void Geometry2DClass::Render(int32_t Length, Vector2D DisplaySize, Number Ratio, Coord2D Transform, bool Mirrored, byte *Layout, Number *Overlay)
{
    Geometries *Type = Values.At<Geometries>(Geometry);
    GeometryOperation *Operation = Values.At<GeometryOperation>(Value::Operation);
    Number *Fade = Values.At<Number>(Value::Fade);
    Coord2D *Position = Values.At<Coord2D>(Value::Position);
    Vector2D *Size = Values.At<Vector2D>(Value::Size);

    if (Type == nullptr || Operation == nullptr)
    {
        ReportError(Status::MissingModule);
        return;
    }

    if (*Type == Geometries::Fill)
    {
        for (int32_t Index = 0; Index < Length; Index++)
            Overlay[Index] = 1;
    }
    else
    {
        Transform = Transform.Join(*Position);
        for (int32_t Y = 0; Y < int32_t(DisplaySize.Y); Y++)
        {
            for (int32_t X = 0; X < int32_t(DisplaySize.X); X++)
            {
                int32_t Index = (DisplaySize.Y - Y - 1) * DisplaySize.X + X; // Invert Y due to layout coords |''
                if (Layout[Index] == 0)
                    continue;
                Index = Layout[Index] - 1;

                if (*Operation == GeometryOperation::Add && Overlay[Index] >= Number(1))
                    continue;
                else if ((*Operation == GeometryOperation::Cut || *Operation == GeometryOperation::Intersect) && Overlay[Index] <= Number(0))
                    continue;

                Vector2D Centered = Transform.TransformTo(Vector2D(X, Y));
                if (Mirrored)
                    Centered = Centered.Mirror(Vector2D(0, 1));

                // Calculate shape
                Number LocalOverlay = 0;

                switch (*Type)
                {
                case Geometries::Box:
                    if (Position == nullptr || Size == nullptr || Fade == nullptr)
                        break;
                    LocalOverlay = min(Size->X - abs(Centered.X), Size->Y - abs(Centered.Y)) / *Fade + 0.5;
                    break;
                case Geometries::Triangle:
                    if (Position == nullptr || Size == nullptr || Fade == nullptr)
                        break;
                    // 2h|x|/w + y < h
                    LocalOverlay = min(Size->Y - Centered.Y - 2 * Size->Y * abs(Centered.X) / Size->X, Centered.Y) / *Fade + 0.5;
                    break;
                case Geometries::Elipse:
                    // Size is radius
                    if (Position == nullptr || Size == nullptr || Fade == nullptr)
                        break;

                    if (abs(Centered.X) > Size->X + *Fade || abs(Centered.Y) > Size->Y + *Fade) // Fast box, because sqrt is slow
                        LocalOverlay = 0;
                    else if (abs(Centered.X) < Size->X * 0.7 - *Fade && abs(Centered.Y) < Size->Y * 0.7 - *Fade) // Fast inner rectangle (of side size 2*r*sqrt(2))
                        LocalOverlay = 1;
                    else
                        LocalOverlay = (1 - sqrt(sq(Centered.X) / sq(Size->X) + sq(Centered.Y) / sq(Size->Y))) / *Fade + 0.5;
                    break;
                case Geometries::DoubleParabola:
                    if (Position == nullptr || Size == nullptr || Fade == nullptr)
                        break;
                    LocalOverlay = -(abs(Centered.X) - Size->X + sq(Centered.Y) * Size->X / sq(Size->Y)) / *Fade + 0.5;
                    break;
                case Geometries::HalfFill:
                    if (Position == nullptr || Fade == nullptr)
                        break;
                    LocalOverlay = Centered.Y / *Fade + 0.5;
                    break;
                default:
                    ReportError(Status::InvalidValue, "Shape");
                    break;
                }

                LocalOverlay = LimitZeroToOne(LocalOverlay);

                if (*Operation == GeometryOperation::Add)
                    Overlay[Index] = Overlay[Index] + LocalOverlay;
                else if (*Operation == GeometryOperation::Cut)
                    Overlay[Index] = Overlay[Index] - LocalOverlay;
                else if (*Operation == GeometryOperation::Intersect)
                    Overlay[Index] = Overlay[Index] * LocalOverlay;
                else if (*Operation == GeometryOperation::XOR)
                    Overlay[Index] = max(Overlay[Index], LocalOverlay) - min(Overlay[Index], LocalOverlay);

                Overlay[Index] = LimitZeroToOne(Overlay[Index]);
            }
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