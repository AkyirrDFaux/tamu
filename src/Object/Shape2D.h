class Shape2DClass : public BaseClass
{
public:
    enum Module
    {
        Texture,
    };

    Shape2DClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~Shape2DClass();

    void Render(int32_t Length, Vector2D Size, Number Ratio, Coord2D Transform, bool Mirrored, byte *Layout, ColourClass *Buffer);
};

Shape2DClass::Shape2DClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    Type = ObjectTypes::Shape2D;
    Name = "Shape";
};
Shape2DClass::~Shape2DClass() {
};

void Shape2DClass::Render(int32_t Length, Vector2D Size, Number Ratio, Coord2D Transform, bool Mirrored, byte *Layout, ColourClass *Buffer)
{
    if (Flags == Flags::Inactive)
        return;

    Texture2D *Texture = Modules.Get<Texture2D>(Module::Texture);
    if (Texture == nullptr)
        return;

    Number Overlay[Length];
    //Calculate overlays
    for (int32_t Index = Modules.FirstValid(ObjectTypes::Geometry2D); Index < Modules.Length; Modules.Iterate(&Index, ObjectTypes::Geometry2D))
        Modules[Index]->As<Geometry2DClass>()->Render(Length, Size, Ratio, Transform, Mirrored, Layout, Overlay);

    //Apply texture
    Texture->Render(Length, Size, Ratio, Transform, Mirrored, Layout, Overlay, Buffer);
    return;
};