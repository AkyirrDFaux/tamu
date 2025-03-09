class Geometry2DClass : public Variable<Geometries>
{
public:
    enum Module
    {
        Operation,
        Fade,
        Position,
        Size
    };

    Geometry2DClass(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup();

    float Render(float Previous, Vector2D PixelPosition);
};

Geometry2DClass::Geometry2DClass(bool New, IDClass ID, FlagClass Flags) : Variable(Geometries::None, ID, Flags)
{
    Type = Types::Geometry2D;
    Name = "Geometry";
    if (New)
    {
        AddModule(new Variable<GeometryOperation>(GeometryOperation::Add), Module::Operation);
        Modules[Module::Operation]->Name = "Operation";
    }
};

void Geometry2DClass::Setup()
{
    switch (*Data)
    {
    case Geometries::Box:
    case Geometries::Elipse:
    case Geometries::DoubleParabola:
        if (!Modules.IsValidID(Size))
        {
            AddModule(new Variable<Vector2D>(Vector2D(1, 1)), Size);
            Modules[Size]->Name = "Size";
        }
    case Geometries::HalfFill:
        if (!Modules.IsValidID(Fade))
        {
            AddModule(new Variable<float>(1), Fade);
            Modules[Fade]->Name = "Fade";
        }
        if (!Modules.IsValidID(Position))
        {
            AddModule(new Variable<Coord2D>(Coord2D()), Position);
            Modules[Position]->Name = "Position";
        }
        break;
    }
};

float Geometry2DClass::Render(float Previous, Vector2D PixelPosition)
{
    GeometryOperation *Operation = Modules.GetValue<GeometryOperation>(Module::Operation);
    float *Fade = Modules.GetValue<float>(Module::Fade);
    Coord2D *Position = Modules.GetValue<Coord2D>(Module::Position);
    Vector2D *Size = Modules.GetValue<Vector2D>(Module::Size);

    if (Operation == nullptr)
    {
        ReportError(Status::MissingModule);
        return Previous;
    }

    Vector2D Local;

    float Overlay = 0;
    switch (*Data)
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
        ReportError(Status::InvalidValue, "Shape : " + String(*ValueAs<uint8_t>()));
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