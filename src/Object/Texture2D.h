class Texture2D : public BaseClass
{
public:
    Texture2D(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup(int32_t Index = -1);
    void Render(int32_t Length, Vector2D Size, Number Ratio, Coord2D Transform, bool Mirrored, byte *Layout, Number *Overlay, ColourClass *Buffer);

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
    BaseClass::Type = ObjectTypes::Texture2D;
    Name = "Texture";

    ValueSet(Textures2D::None);
};

void Texture2D::Setup(int32_t Index)
{
    if (Index != -1 && Index != 0)
        return;

    if (Values.Type(Texture) != Types::Texture2D)
        return;

    switch (ValueGet<Textures2D>(Texture))
    {
    case Textures2D::BlendLinear:
    case Textures2D::BlendCircular:
        if (Values.Type(ColourB) != Types::Colour)
            ValueSet(ColourClass(0, 0, 0, 255), ColourB);
        if (Values.Type(Position) != Types::Coord2D)
            ValueSet(Coord2D(), Position);
        if (Values.Type(Width) != Types::Number)
            ValueSet<Number>(1, Width);
    case Textures2D::Full:
        if (Values.Type(ColourA) != Types::Colour)
            ValueSet(ColourClass(0, 0, 0, 255), ColourA);
        break;
    default:
        break;
    }
};

void Texture2D::Render(int32_t Length, Vector2D Size, Number Ratio, Coord2D Transform, bool Mirrored, byte *Layout, Number *Overlay, ColourClass *Buffer)
{
    if (Values.Type(Texture) != Types::Texture2D)
    {
        ReportError(Status::MissingModule , "Texture 2D");
        return;
    }

    Textures2D Texture = ValueGet<Textures2D>(Value::Texture);
    ColourClass ColourA;
    ColourClass ColourB;
    Coord2D Position;
    Number Width;

    switch (Texture)
    {
    case Textures2D::BlendLinear:
    case Textures2D::BlendCircular:
        if (Values.Type(Value::ColourA) != Types::Colour || Values.Type(Value::ColourB) != Types::Colour ||
            Values.Type(Value::Position) != Types::Coord2D || Values.Type(Value::Width) != Types::Number)
            return;
        ColourA = ValueGet<ColourClass>(Value::ColourA);
        ColourB = ValueGet<ColourClass>(Value::ColourB);
        Position = ValueGet<Coord2D>(Value::Position);
        Width = ValueGet<Number>(Value::Width);
        break;
    case Textures2D::Full:
        if (Values.Type(Value::ColourA) != Types::Colour)
            return;
        ColourA = ValueGet<ColourClass>(Value::ColourA);
        for (int32_t Index = 0; Index < Length; Index++)
        {
            if (Overlay[Index] <= Number(0))
                continue;
            Buffer[Index].Layer(ColourA, Overlay[Index]);
        }
        return;
    default:
        break;
    }

    Transform = Transform.Join(Position);
    for (int32_t Y = 0; Y < int32_t(Size.Y); Y++)
    {
        for (int32_t X = 0; X < int32_t(Size.X); X++)
        {
            int32_t Index = (Size.Y - Y - 1) * Size.X + X; // Invert Y due to layout coords |''
            if (Layout[Index] == 0)
                continue;
            Index = Layout[Index] - 1;

            if (Overlay[Index] <= Number(0)) // Skip if not visible
                continue;

            Vector2D Centered = Transform.TransformTo(Vector2D(X, Y));
            if (Mirrored)
                Centered = Centered.Mirror(Vector2D(0, 1));

            ColourClass Colour;
            Number Distance;

            switch (Texture)
            {
            case Textures2D::BlendLinear:
                Distance = Centered.X;
                Distance = LimitZeroToOne((Distance / Width / 2) + 0.5);
                Colour = ColourB;
                Colour.Layer(ColourA, Distance);
                break;
            case Textures2D::BlendCircular: // Untested
                Distance = Centered.Length();
                Distance = LimitZeroToOne((Distance / Width / 2) + 0.5);
                Colour = ColourB;
                Colour.Layer(ColourA, Distance);
                break;
            default:
                ReportError(Status::InvalidValue, "Texture");
                break;
            }
            Buffer[Index].Layer(Colour, Overlay[Index]);
        }
    }
    return;
};