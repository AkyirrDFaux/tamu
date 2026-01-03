class Texture1D : public BaseClass
{
public:
    Texture1D(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup(int32_t Index = -1);
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
    BaseClass::Type = ObjectTypes::Texture1D;
    Values.Add(Textures1D::None);
    Name = "Texture";
};

void Texture1D::Setup(int32_t Index)
{
    if (Index != -1 && Index != 0)
        return;
        
    Textures1D *Type = Values.At<Textures1D>(TextureType);
    switch (*Type)
    {
    case Textures1D::Point:
    case Textures1D::Blend:
        if (!Values.IsValid(ColourB))
            Values.Add(ColourClass(0, 0, 0, 255), ColourB);
        if (!Values.IsValid(Position))
            Values.Add<Number>(0, Position);
        if (!Values.IsValid(Width))
            Values.Add<Number>(1, Width);

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
    Number *Position = Values.At<Number>(Value::Position);
    Number *Width = Values.At<Number>(Value::Width);

    ColourClass Colour;
    Number Distance;

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
    case Textures1D::Point:
        if (ColourA == nullptr || ColourB == nullptr || Position == nullptr || Width == nullptr)
            break;
        Distance = abs(PixelPosition - *Position);
        Distance = LimitZeroToOne((Distance / *Width / 2) + 0.5);
        Colour = *ColourB;
        Colour.Layer(*ColourA, Distance);
        break;
    }
    return Colour;
};