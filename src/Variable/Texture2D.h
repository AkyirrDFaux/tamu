class Texture2D : public Variable<Textures2D>
{
public:
    Texture2D(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup();
    ColourClass Render(Vector2D PixelPosition);

    enum Module
    {
        ColourA,
        ColourB,
        Position,
        Width
    };
};

Texture2D::Texture2D(bool New, IDClass ID, FlagClass Flags) : Variable(Textures2D::None, ID, Flags)
{
    BaseClass::Type = Types::Texture2D;
    Name = "Texture";
};

void Texture2D::Setup()
{
    switch (*Data)
    {
    case Textures2D::BlendLinear:
    case Textures2D::BlendCircular:
        if (!Modules.IsValidID(ColourB))
        {
            AddModule(new Variable<ColourClass>(ColourClass(0, 0, 0, 255)), Module::ColourB);
            Modules[Module::ColourB]->Name = "Colour B";
        }
        if (!Modules.IsValidID(Position))
        {
            AddModule(new Variable<Coord2D>(Coord2D()), Module::Position);
            Modules[Module::Position]->Name = "Position";
        }
        if (!Modules.IsValidID(Width))
        {
            AddModule(new Variable<float>(1), Module::Width);
            Modules[Module::Width]->Name = "Width";
        }
    case Textures2D::Full:
        if (!Modules.IsValidID(ColourA))
        {
            AddModule(new Variable<ColourClass>(ColourClass(0, 0, 0, 255)), Module::ColourA);
            Modules[Module::ColourA]->Name = "Colour A";
        }
        break;
    default:
        break;
    }
};

ColourClass Texture2D::Render(Vector2D PixelPosition)
{
    Textures2D *Texture = ValueAs<Textures2D>();
    ColourClass *ColourA = Modules.GetValue<ColourClass>(Module::ColourA);
    ColourClass *ColourB = Modules.GetValue<ColourClass>(Module::ColourB);
    Coord2D *Position = Modules.GetValue<Coord2D>(Module::Position);
    float *Width = Modules.GetValue<float>(Module::Width);

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