class Texture1D : public Variable<Textures1D>
{
public:
    Texture1D(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup();
    ColourClass Render(int32_t PixelPosition);

    enum Module
    {
        ColourA,
        ColourB,
        Position,
        Width
    };
};

Texture1D::Texture1D(bool New, IDClass ID, FlagClass Flags) : Variable(Textures1D::None, ID, Flags)
{
    BaseClass::Type = Types::Texture1D;
    Name = "Texture";
};

void Texture1D::Setup()
{
    switch (*Data)
    {
    case Textures1D::Blend:
        if (!Modules.IsValidID(ColourB))
        {
            AddModule(new Variable<ColourClass>(ColourClass(0, 0, 0, 255)), Module::ColourB);
            Modules[Module::ColourB]->Name = "Colour B";
        }
        if (!Modules.IsValidID(Position))
        {
            AddModule(new Variable<float>(0), Module::Position);
            Modules[Module::Position]->Name = "Position";
        }
        if (!Modules.IsValidID(Width))
        {
            AddModule(new Variable<float>(1), Module::Width);
            Modules[Module::Width]->Name = "Width";
        }
    case Textures1D::Full:
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

ColourClass Texture1D::Render(int32_t PixelPosition)
{
    Textures1D *Texture = ValueAs<Textures1D>();
    ColourClass *ColourA = Modules.GetValue<ColourClass>(Module::ColourA);
    ColourClass *ColourB = Modules.GetValue<ColourClass>(Module::ColourB);
    float *Position = Modules.GetValue<float>(Module::Position);
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