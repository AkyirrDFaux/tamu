// Replace colour with texture

class Shape2DClass : public BaseClass
{
public:
    enum Module
    {
        Texture,
    };

    Shape2DClass(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~Shape2DClass();

    ColourClass Render(ColourClass Current, Vector2D Position);
};

Shape2DClass::Shape2DClass(bool New, IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    Type = Types::Shape2D;
    Name = "Shape";
    if (New)
    {
        AddModule(new Texture2D(), Texture);
    }
};
Shape2DClass::~Shape2DClass() {
};

ColourClass Shape2DClass::Render(ColourClass Current, Vector2D Position)
{
    Texture2D *Texture = Modules.Get<Texture2D>(Module::Texture);

    if (Texture == nullptr)
        return Current;

    float Overlay = 0;

    for (int32_t Index = Modules.FirstValid(Types::Geometry2D); Index < Modules.Length; Modules.Iterate(&Index,Types::Geometry2D))
            Overlay = Modules[Index]->As<Geometry2DClass>()->Render(Overlay, Position);

    Current.Layer(Texture->Render(Position), Overlay);
    return Current;
};