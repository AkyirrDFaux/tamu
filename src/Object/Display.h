class DisplayClass : public Variable<Displays>
{
public:
    byte *Layout = nullptr;
    enum Module
    {
        DisplayPort,
        Length,
        Size,
        Ratio,
        Brightness,
        Offset,
        Mirrored,
        Parts
    };

    DisplayClass(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~DisplayClass();

    void Setup();
    bool Run();
    ColourClass RenderPixel(Vector2D Base);
};

DisplayClass::DisplayClass(bool New, IDClass ID, FlagClass Flags) : Variable(Displays::Undefined, ID, Flags)
{
    BaseClass::Type = Types::Display;
    Name = "Display";
    Outputs.Add(this);

    if (!Modules.IsValidID(Length))
    {
        AddModule(new Variable<int32_t>(0, RandomID, Flags::Auto), Length);
        Modules[Length]->Name = "Length";
    }
    if (!Modules.IsValidID(Size))
    {
        AddModule(new Variable<Vector2D>(Vector2D(0, 0), RandomID, Flags::Auto), Size);
        Modules[Size]->Name = "Size";
    }
    if (!Modules.IsValidID(Ratio))
    {
        AddModule(new Variable<float>(1, RandomID, Flags::Auto), Ratio);
        Modules[Ratio]->Name = "Ratio";
    }

    if (New)
    {
        AddModule(new PortAttachClass(New), DisplayPort);
        AddModule(new Variable<uint8_t>(20), Brightness);
        Modules[Brightness]->Name = "Brightness";
        AddModule(new Variable<Coord2D>(Coord2D(0, 0, 0)), Offset);
        Modules[Offset]->Name = "Offset";
        AddModule(new Variable<bool>(false), Mirrored);
        Modules[Mirrored]->Name = "Mirrored";
        AddModule(new Folder(true), Parts);
        Modules[Parts]->Name = "Parts";
    }
};

void DisplayClass::Setup()
{
    int32_t *Length = Modules.GetValue<int32_t>(Module::Length);
    Vector2D *Size = Modules.GetValue<Vector2D>(Module::Size);
    float *Ratio = Modules.GetValue<float>(Module::Ratio);

    if (Length == nullptr || Size == nullptr || Ratio == nullptr)
        return;

    if (*Data == Displays::Vysi_v1_0)
    {
        *Length = 86;
        *Ratio = 1;
        *Size = Vector2D(11, 10);
        Layout = LayoutVysiv1_0;
    }
};

DisplayClass::~DisplayClass()
{
    Outputs.Remove(this);
};

bool DisplayClass::Run()
{
    PortAttachClass *DisplayPort = Modules.Get<PortAttachClass>(Module::DisplayPort); // HW connection
    int32_t *Length = Modules.GetValue<int32_t>(Module::Length);
    Vector2D *Size = Modules.GetValue<Vector2D>(Module::Size);
    float *Ratio = Modules.GetValue<float>(Module::Ratio);
    uint8_t *Brightness = Modules.GetValue<uint8_t>(Module::Brightness);
    Coord2D *Offset = Modules.GetValue<Coord2D>(Module::Offset);
    bool *Mirrored = Modules.GetValue<bool>(Module::Mirrored);

    if (DisplayPort == nullptr || Length == nullptr || Size == nullptr || Ratio == nullptr ||
        Brightness == nullptr || Offset == nullptr || Mirrored == nullptr || Layout == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }
    if (DisplayPort->Check(Drivers::LED) == false)
    {
        ReportError(Status::PortError);
        return true;
    }

    CRGB *Pixel = (CRGB *)DisplayPort->Port->Driver;
    //Pixel->clear();

    Coord2D Transform = Coord2D(*Size * 0.5 - Vector2D(0.5, 0.5), 0).Join(*Offset);
    // Iterate over corrected pixel coords |_
    for (int32_t Y = 0; Y < Size->Y; Y++)
    {
        for (int32_t X = 0; X < Size->X; X++)
        {
            int32_t Index = (Size->Y - Y - 1) * Size->X + X; // Invert Y due to layout coords |''
            if (Layout[Index] == 0)
                continue;

            Vector2D Centered = Transform.TransformTo(Vector2D(X, Y));
            if (*Mirrored)
                Centered = Centered.Mirror(Vector2D(0, 1));

            ColourClass PixelColour = RenderPixel(Centered);
            PixelColour.ToDisplay(*Brightness);
            Pixel[Layout[Index] - 1].setRGB(PixelColour.R, PixelColour.G, PixelColour.B);
        }
    }
    return true;
};

ColourClass DisplayClass::RenderPixel(Vector2D Centered)
{
    Folder *Parts = Modules.Get<Folder>(Module::Parts);

    ColourClass Colour = ColourClass(0, 0, 0);
    if (Parts == nullptr)
    {
        ReportError(Status::MissingModule);
        return Colour;
    }

    for (int32_t Index = Parts->Modules.FirstValid(Types::Shape2D); Index < Parts->Modules.Length; Parts->Modules.Iterate(&Index, Types::Shape2D))
        Colour = Parts->Modules[Index]->As<Shape2DClass>()->Render(Colour, Centered);
    return Colour;
};