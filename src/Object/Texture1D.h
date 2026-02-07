class Texture1D : public BaseClass
{
public:
    Texture1D(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup(int32_t Index = -1);

    static void SetupBridge(BaseClass *Base, int32_t Index) { static_cast<Texture1D *>(Base)->Setup(Index); }
    static constexpr VTable Table = {
        .Setup = Texture1D::SetupBridge,
        .Run = BaseClass::DefaultRun,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};

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

constexpr VTable Texture1D::Table;

Texture1D::Texture1D(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags)
{
    BaseClass::Type = ObjectTypes::Texture1D;
    ValueSet(Textures1D::None);
    Name = "Texture";
};

void Texture1D::Setup(int32_t Index)
{
    if (Index != -1 && Index != 0)
        return;

    if (Values.Type(TextureType) != Types::Texture1D)
        return;

    switch (ValueGet<Textures1D>(TextureType))
    {
    case Textures1D::Point:
    case Textures1D::Blend:
        if (Values.Type(ColourB) != Types::Colour)
            ValueSet(ColourClass(0, 0, 0, 255), ColourB);
        if (Values.Type(Position) != Types::Number)
            ValueSet<Number>(0, Position);
        if (Values.Type(Width) != Types::Number)
            ValueSet<Number>(1, Width);

    case Textures1D::Full:
        if (Values.Type(ColourA) != Types::Colour)
            ValueSet(ColourClass(0, 0, 0, 255), ColourA);
        break;
    default:
        break;
    }
};

ColourClass Texture1D::Render(int32_t PixelPosition)
{
    if (Values.Type(TextureType) != Types::Texture1D)
    {
        ReportError(Status::MissingModule, "Texture 1D");
        return ColourClass();
    }

    Textures1D Type = ValueGet<Textures1D>(TextureType);
    ColourClass ColourA;
    ColourClass ColourB;
    Number Position;
    Number Width;

    switch (ValueGet<Textures1D>(TextureType))
    {
    case Textures1D::Point:
    case Textures1D::Blend:
        if (Values.Type(Value::ColourB) != Types::Colour || Values.Type(Value::Position) != Types::Number || Values.Type(Value::Width) != Types::Number)
            return ColourClass();
        ColourB = ValueGet<ColourClass>(Value::ColourB);
        Position = ValueGet<Number>(Value::Position);
        Width = ValueGet<Number>(Value::Width);
    case Textures1D::Full:
        if (Values.Type(Value::ColourA) != Types::Colour)
            return ColourClass();
        ColourB = ValueGet<ColourClass>(Value::ColourA);
        break;
    default:
        break;
    }

    ColourClass Colour;
    Number Distance;

    switch (Type)
    {
    case Textures1D::Full:
        Colour = ColourA;
        break;
    case Textures1D::Blend:
        Distance = PixelPosition - Position;
        Distance = LimitZeroToOne((Distance / Width / 2) + 0.5);
        Colour = ColourB;
        Colour.Layer(ColourA, Distance);
        break;
    case Textures1D::Point:
        Distance = abs(PixelPosition - Position);
        Distance = LimitZeroToOne((Distance / Width / 2) + 0.5);
        Colour = ColourB;
        Colour.Layer(ColourA, Distance);
        break;
    default:
        break;
    }
    return Colour;
};