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

    // Iterate over corrected pixel coords |_
    /*for (int32_t Y = 0; Y < int32_t(Size.Y); Y++)
    {
        for (int32_t X = 0; X < int32_t(Size.X); X++)
        {
            int32_t Index = (Size.Y - Y - 1) * Size.X + X; // Invert Y due to layout coords |''
            if (Layout[Index] == 0)
                continue;
            Index = Layout[Index] - 1;

            Vector2D Centered = Transform.TransformTo(Vector2D(X, Y));
            if (Mirrored)
                Centered = Centered.Mirror(Vector2D(0, 1));
        }
    }*/
    return;
};