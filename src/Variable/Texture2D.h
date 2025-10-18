class Texture2D : public BaseClass
{
public:
    Texture2D(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup();
    ColourClass Render(Vector2D PixelPosition);

    enum Value
    {
        Texture,
        ColourA,
        ColourB,
        Position,
        Width
    };
};

Texture2D::Texture2D(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    BaseClass::Type = Types::Texture2D;
    Name = "Texture";

    Values.Add(Textures2D::None);
};

void Texture2D::Setup()
{
    Textures2D *Type = Values.At<Textures2D>(Texture);
    switch (*Type)
    {
    case Textures2D::BlendLinear:
    case Textures2D::BlendCircular:
        if (!Values.IsValid(ColourB))
            Values.Add(ColourClass(0, 0, 0, 255), ColourB);
        if (!Values.IsValid(Position))
            Values.Add(Coord2D(), Position);
        if (!Values.IsValid(Width))
            Values.Add<float>(1, Width);
    case Textures2D::Full:
        if (!Values.IsValid(ColourA))
            Values.Add(ColourClass(0, 0, 0, 255), ColourA);
        break;
    default:
        break;
    }
};

ColourClass Texture2D::Render(Vector2D PixelPosition)
{
    Textures2D *Texture = Values.At<Textures2D>(Value::Texture);
    ColourClass *ColourA = Values.At<ColourClass>(Value::ColourA);
    ColourClass *ColourB = Values.At<ColourClass>(Value::ColourB);
    Coord2D *Position = Values.At<Coord2D>(Value::Position);
    float *Width = Values.At<float>(Value::Width);

    ColourClass Colour;
    float Distance;

    if (Texture == nullptr)
    {
        ReportError(Status::MissingModule);
        return Colour;
    }

    switch (*Texture)
    {
    case Textures2D::Full:
        if (ColourA == nullptr)
            break;
        Colour = *ColourA;
        break;
    case Textures2D::BlendLinear:
        if (ColourA == nullptr || ColourB == nullptr || Position == nullptr || Width == nullptr)
            break;
        Distance = Position->TransformTo(PixelPosition).X;
        Distance = LimitZeroToOne((Distance / *Width / 2) + 0.5);
        Colour = *ColourB;
        Colour.Layer(*ColourA, Distance);
        break;
    case Textures2D::BlendCircular: // Untested
        Distance = Position->TransformTo(PixelPosition).Length();
        Distance = LimitZeroToOne((Distance / *Width / 2) + 0.5);
        Colour = *ColourB;
        Colour.Layer(*ColourA, Distance);
        break;
    }
    return Colour;
};