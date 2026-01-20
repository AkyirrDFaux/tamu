class DisplayClass : public BaseClass
{
public:
    byte *Layout = nullptr;
    CRGB *LED = nullptr;
    enum Value
    {
        DisplayType,
        Length,
        Size,
        Ratio,
        Brightness,
        Offset,
        Mirrored
    };

    DisplayClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~DisplayClass();

    void Setup(int32_t Index = -1);
    bool Run();
    ColourClass RenderPixel(Vector2D Base);
};

DisplayClass::DisplayClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    BaseClass::Type = ObjectTypes::Display;
    Name = "Display";
    Outputs.Add(this);

    Values.Add(Displays::Undefined, DisplayType);
    Values.Add<int32_t>(0, Length);
    Values.Add(Vector2D(0, 0), Size);
    Values.Add<Number>(1, Ratio);
    Values.Add<uint8_t>(20, Brightness);
    Values.Add(Coord2D(0, 0, 0), Offset);
    Values.Add(false, Mirrored);
};

void DisplayClass::Setup(int32_t Index)
{
    if (Index != -1 && Index != 0)
        return;

    Displays *Type = Values.At<Displays>(Value::DisplayType);
    int32_t *Length = Values.At<int32_t>(Value::Length);
    Vector2D *Size = Values.At<Vector2D>(Value::Size);
    Number *Ratio = Values.At<Number>(Value::Ratio);

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
    int32_t *Length = Values.At<int32_t>(Value::Length);
    Vector2D *Size = Values.At<Vector2D>(Value::Size);
    Number *Ratio = Values.At<Number>(Value::Ratio);
    uint8_t *Brightness = Values.At<uint8_t>(Value::Brightness);
    Coord2D *Offset = Values.At<Coord2D>(Value::Offset);
    bool *Mirrored = Values.At<bool>(Value::Mirrored);

    if (Length == nullptr || Size == nullptr || Ratio == nullptr ||
        Brightness == nullptr || Offset == nullptr || Mirrored == nullptr || Layout == nullptr)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    if (LED == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }

    ColourClass Buffer[*Length];
    Coord2D Transform = Coord2D(*Size * 0.5 - Vector2D(0.5, 0.5), Vector2D(0)).Join(*Offset);
    //Iterate over layers
    for (int32_t Index = Modules.FirstValid(ObjectTypes::Shape2D); Index < Modules.Length; Modules.Iterate(&Index, ObjectTypes::Shape2D))
        Modules[Index]->As<Shape2DClass>()->Render(*Length, *Size, *Ratio, Transform, *Mirrored, Layout, Buffer);

    //Final output
    for (int32_t Index = 0; Index < *Length; Index++)
    {
        Buffer[Index].ToDisplay(*Brightness);
        LED[Index].setRGB(Buffer[Index].R, Buffer[Index].G, Buffer[Index].B);
    }     
    
    return true;
};

ColourClass DisplayClass::RenderPixel(Vector2D Centered) {

};