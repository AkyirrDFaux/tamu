class Texture1D : public BaseClass
{
public:
    Texture1D(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup();
    ColourClass Render(int32_t PixelPosition);

    enum Value
    {
        TextureType,
        ColourA,
        ColourB,
        Position,
        Width
    };
};

Texture1D::Texture1D(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    BaseClass::Type = Types::Texture1D;
    Values.Add(Textures1D::None);
    Name = "Texture";
};

void Texture1D::Setup()
{
    Textures1D *Type = Values.At<Textures1D>(TextureType);
    switch (*Type)
    {
    case Textures1D::Blend:
        if (!Values.IsValid(ColourB))
            Values.Add(ColourClass(0, 0, 0, 255), ColourB);
        if (!Values.IsValid(Position))
            Values.Add<float>(0, Position);
        if (!Values.IsValid(Width))
            Values.Add<float>(1, Width);

    case Textures1D::Full:
        if (!Values.IsValid(ColourA))
            Values.Add(ColourClass(0, 0, 0, 255), ColourA);
        break;
    default:
        break;
    }
};

ColourClass Texture1D::Render(int32_t PixelPosition)
{
    Textures1D *Type = Values.At<Textures1D>(TextureType);
    ColourClass *ColourA = Values.At<ColourClass>(Value::ColourA);
    ColourClass *ColourB = Values.At<ColourClass>(Value::ColourB);
    float *Position = Values.At<float>(Value::Position);
    float *Width = Values.At<float>(Value::Width);

    ColourClass Colour;
    float Distance;

    if (Type == nullptr)
    {
        ReportError(Status::MissingModule);
        return Colour;
    }

    switch (*Type)
    {
    case Textures1D::Full:
        if (ColourA == nullptr)
            break;
        Colour = *ColourA;
        break;
    case Textures1D::Blend:
        if (ColourA == nullptr || ColourB == nullptr || Position == nullptr || Width == nullptr)
            break;
        Distance = PixelPosition - *Position;
        Distance = LimitZeroToOne((Distance / *Width / 2) + 0.5);
        Colour = *ColourB;
        Colour.Layer(*ColourA, Distance);
        break;
    }
    return Colour;
};