class Shape2DClass : public BaseClass
{
public:
    enum Module
    {
        Texture,
    };

    Shape2DClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~Shape2DClass();

    ColourClass Render(ColourClass Current, Vector2D Position);
};

Shape2DClass::Shape2DClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    Type = Types::Shape2D;
    Name = "Shape";
};
Shape2DClass::~Shape2DClass() {
};

ColourClass Shape2DClass::Render(ColourClass Current, Vector2D Position)
{
    if (Flags == Flags::Inactive)
        return Current;

    Texture2D *Texture = Modules.Get<Texture2D>(Module::Texture);

    if (Texture == nullptr)
        return Current;

    float Overlay = 0;

    for (int32_t Index = Modules.FirstValid(Types::Geometry2D); Index < Modules.Length; Modules.Iterate(&Index,Types::Geometry2D))
            Overlay = Modules[Index]->As<Geometry2DClass>()->Render(Overlay, Position);

    Current.Layer(Texture->Render(Position), Overlay);
    return Current;
};