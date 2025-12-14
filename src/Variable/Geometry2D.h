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
    void Setup();

    float Render(float Previous, Vector2D PixelPosition);
};

Geometry2DClass::Geometry2DClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    Type = ObjectTypes::Geometry2D;
    Name = "Geometry";

    Values.Add(Geometries::None);
    Values.Add(GeometryOperation::Add);
};

void Geometry2DClass::Setup()
{
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
            Values.Add<float>(1, Fade);
        if (!Values.IsValid(Position))
            Values.Add(Coord2D(), Position);
        break;
    }
};

float Geometry2DClass::Render(float Previous, Vector2D PixelPosition)
{
    Geometries *Type = Values.At<Geometries>(Geometry);
    GeometryOperation *Operation = Values.At<GeometryOperation>(Value::Operation);
    float *Fade = Values.At<float>(Value::Fade);
    Coord2D *Position = Values.At<Coord2D>(Value::Position);
    Vector2D *Size = Values.At<Vector2D>(Value::Size);

    if (Operation == nullptr)
    {
        ReportError(Status::MissingModule);
        return Previous;
    }

    Vector2D Local;

    float Overlay = 0;
    switch (*Type)
    {
    case Geometries::Fill:
        Overlay = 1;
        break;
    case Geometries::Box:
        if (Position == nullptr || Size == nullptr || Fade == nullptr)
            break;
        Local = Position->TransformTo(PixelPosition);
        Overlay = min(Size->X - abs(Local.X), Size->Y - abs(Local.Y)) / *Fade + 0.5;
        break;
    case Geometries::Triangle:
        if (Position == nullptr || Size == nullptr || Fade == nullptr)
            break;
        Local = Position->TransformTo(PixelPosition);
        //2h|x|/w + y < h
        Overlay = min(Size->Y - Local.Y - 2 * Size->Y * abs(Local.X) / Size->X, Local.Y) / *Fade + 0.5;
        break;
    case Geometries::Elipse:
        if (Position == nullptr || Size == nullptr || Fade == nullptr)
            break;
        Local = Position->TransformTo(PixelPosition);
        Overlay = (1 - sqrt(sq(Local.X) / sq(Size->X) + sq(Local.Y) / sq(Size->Y))) / *Fade + 0.5;
        break;
    case Geometries::DoubleParabola:
        if (Position == nullptr || Size == nullptr || Fade == nullptr)
            break;
        Local = Position->TransformTo(PixelPosition);
        Overlay = -(abs(Local.X) - Size->X + sq(Local.Y) * Size->X / sq(Size->Y)) / *Fade + 0.5;
        break;
    case Geometries::HalfFill: // Untested
        if (Position == nullptr || Fade == nullptr)
            break;
        Local = Position->TransformTo(PixelPosition);
        Overlay = Local.Y / *Fade + 0.5;
        break;
    default:
        ReportError(Status::InvalidValue, "Shape");
        break;
    }

    Overlay = LimitZeroToOne(Overlay);

    if (*Operation == GeometryOperation::Add)
        Overlay = Previous + Overlay;
    else if (*Operation == GeometryOperation::Cut)
        Overlay = Previous - Overlay;
    else if (*Operation == GeometryOperation::Intersect)
        Overlay = Previous * Overlay;
    else if (*Operation == GeometryOperation::XOR)
        Overlay = max(Previous, Overlay) - min(Previous, Overlay);

    return LimitZeroToOne(Overlay);
}

/*
float DoubleParabola(CoordClass Current, CoordClass Center,
                     float Width, float Height, float Fade)
{
    float Distance = (abs(Current.X - Center.X) - Width + Square(Current.Y - Center.Y) / (Square(Height) / Width)) / Fade;

    return 1 - LimitZeroToOne(Distance);
}
*/