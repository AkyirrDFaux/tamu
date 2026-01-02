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

    Values.Add(Textures2D::None);
};

void Texture2D::Setup(int32_t Index)
{
    if (Index != -1 && Index != 0)
        return;

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
            Values.Add<Number>(1, Width);
    case Textures2D::Full:
        if (!Values.IsValid(ColourA))
            Values.Add(ColourClass(0, 0, 0, 255), ColourA);
        break;
    default:
        break;
    }
};

void Texture2D::Render(int32_t Length, Vector2D Size, Number Ratio, Coord2D Transform, bool Mirrored, byte *Layout, Number *Overlay, ColourClass *Buffer)
{
    Textures2D *Texture = Values.At<Textures2D>(Value::Texture);
    ColourClass *ColourA = Values.At<ColourClass>(Value::ColourA);
    ColourClass *ColourB = Values.At<ColourClass>(Value::ColourB);
    Coord2D *Position = Values.At<Coord2D>(Value::Position);
    Number *Width = Values.At<Number>(Value::Width);

    if (Texture == nullptr || ColourA == nullptr)
    {
        ReportError(Status::MissingModule);
        return;
    }

    if (*Texture == Textures2D::Full)
    {
        for (int32_t Index = 0; Index < Length; Index++)
        {
            if (Overlay[Index] <= Number(0))
                continue;
            Buffer[Index].Layer(*ColourA, Overlay[Index]);
        }
    }
    else
    {
        if (Position == nullptr)
            return;
        Transform = Transform.Join(*Position);
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

                switch (*Texture)
                {
                case Textures2D::BlendLinear:
                    if (ColourB == nullptr || Width == nullptr)
                        break;
                    Distance = Centered.X;
                    Distance = LimitZeroToOne((Distance / *Width / 2) + 0.5);
                    Colour = *ColourB;
                    Colour.Layer(*ColourA, Distance);
                    break;
                case Textures2D::BlendCircular: // Untested
                    if (ColourB == nullptr || Width == nullptr)
                        break;
                    Distance = Centered.Length();
                    Distance = LimitZeroToOne((Distance / *Width / 2) + 0.5);
                    Colour = *ColourB;
                    Colour.Layer(*ColourA, Distance);
                    break;
                default:
                    ReportError(Status::InvalidValue, "Texture");
                    break;
                }
                Buffer[Index].Layer(Colour, Overlay[Index]);
            }
        }
    }
    return;
};