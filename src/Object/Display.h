class DisplayClass : public BaseClass
{
public:
    byte *Layout = nullptr;
    enum Value{
        DisplayType,
        Length,
        Size,
        Ratio,
        Brightness,
        Offset,
        Mirrored
    };
    enum Module
    {
        Port
    };

    DisplayClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~DisplayClass();

    void Setup();
    bool Run();
    ColourClass RenderPixel(Vector2D Base);
};

DisplayClass::DisplayClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    BaseClass::Type = Types::Display;
    Name = "Display";
    Outputs.Add(this);

    Values.Add(Displays::Undefined,DisplayType);
    Values.Add<int32_t>(0, Length);
    Values.Add(Vector2D(0, 0), Size);
    Values.Add<float>(1, Ratio);
    Values.Add<uint8_t>(20, Brightness);
    Values.Add(Coord2D(0, 0, 0), Offset);
    Values.Add(false, Mirrored);
};

void DisplayClass::Setup()
{
    Displays *Type = Values.At<Displays>(Value::DisplayType);
    int32_t *Length = Values.At<int32_t>(Value::Length);
    Vector2D *Size = Values.At<Vector2D>(Value::Size);
    float *Ratio = Values.At<float>(Value::Ratio);

    if (Type == nullptr || Length == nullptr || Size == nullptr || Ratio == nullptr)
        return;

    if (*Type == Displays::Vysi_v1_0)
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
    PortClass *Port = Modules.Get<PortClass>(Module::Port); // HW connection
    int32_t *Length = Values.At<int32_t>(Value::Length);
    Vector2D *Size = Values.At<Vector2D>(Value::Size);
    float *Ratio = Values.At<float>(Value::Ratio);
    uint8_t *Brightness = Values.At<uint8_t>(Value::Brightness);
    Coord2D *Offset = Values.At<Coord2D>(Value::Offset);
    bool *Mirrored = Values.At<bool>(Value::Mirrored);

    if (Port == nullptr || Length == nullptr || Size == nullptr || Ratio == nullptr ||
        Brightness == nullptr || Offset == nullptr || Mirrored == nullptr || Layout == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }
    
    CRGB *Pixel = Port->GetLED(this);
    if (Pixel == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }

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
    ColourClass Colour = ColourClass(0, 0, 0);

    for (int32_t Index = Modules.FirstValid(Types::Shape2D,1); Index < Modules.Length; Modules.Iterate(&Index, Types::Shape2D))
        Colour = Modules[Index]->As<Shape2DClass>()->Render(Colour, Centered);
    return Colour;
};