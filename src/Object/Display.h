class DisplayClass : public BaseClass
{
public:
    uint8_t *Layout = nullptr;
    LEDDriver LEDs;
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

    static void SetupBridge(BaseClass *Base, int32_t Index) { static_cast<DisplayClass *>(Base)->Setup(Index); }
    static bool RunBridge(BaseClass *Base) { return static_cast<DisplayClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = DisplayClass::SetupBridge,
        .Run = DisplayClass::RunBridge,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};
};

constexpr VTable DisplayClass::Table;

DisplayClass::DisplayClass(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags)
{
    BaseClass::Type = ObjectTypes::Display;
    Name = "Display";
    Outputs.Add(this);

    ValueSet(Displays::Undefined, DisplayType);
    ValueSet<int32_t>(0, Length);
    ValueSet(Vector2D(0, 0), Size);
    ValueSet<Number>(1, Ratio);
    ValueSet<uint8_t>(20, Brightness);
    ValueSet(Coord2D(0, 0, 0), Offset);
    ValueSet(false, Mirrored);
};

void DisplayClass::Setup(int32_t Index)
{
    if (Index != -1 && Index != 0)
        return;

    if (Values.Type(DisplayType) != Types::Display)
        return;

    switch (ValueGet<Displays>(DisplayType))
    {
    case Displays::GenericLEDMatrix:
        ValueSet<int32_t>(256, Length);
        ValueSet<Number>(1, Ratio);
        ValueSet(Vector2D(16, 16), Size);
        Layout = nullptr;
        break;
    case Displays::Vysi_v1_0:
        ValueSet<int32_t>(86, Length);
        ValueSet<Number>(1, Ratio);
        ValueSet(Vector2D(11, 10), Size);
        Layout = LayoutVysiv1_0;
        break;
    default:
        break;
    }
};

DisplayClass::~DisplayClass()
{
    Outputs.Remove(this);
};

bool DisplayClass::Run()
{
    if (Values.Type(Length) != Types::Integer || Values.Type(Size) != Types::Vector2D || Values.Type(Ratio) != Types::Number ||
        Values.Type(Brightness) != Types::Byte || Values.Type(Offset) != Types::Coord2D || Values.Type(Mirrored) != Types::Bool)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    if (LEDs.LEDs == nullptr)
    {
        ReportError(Status::PortError);
        return true;
    }

    uint32_t Length = ValueGet<int32_t>(Value::Length);
    Vector2D Size = ValueGet<Vector2D>(Value::Size);
    Number Ratio = ValueGet<Number>(Value::Ratio);
    uint8_t Brightness = ValueGet<uint8_t>(Value::Brightness);
    Coord2D Offset = ValueGet<Coord2D>(Value::Offset);
    bool Mirrored = ValueGet<bool>(Value::Mirrored);

    ColourClass Buffer[Length];
    Coord2D Transform = Coord2D(Size * 0.5 - Vector2D(0.5, 0.5), Vector2D(0)).Join(Offset);
    // Iterate over layers
    for (uint32_t Index = Modules.FirstValid(ObjectTypes::Shape2D); Index < Modules.Length; Modules.Iterate(&Index, ObjectTypes::Shape2D))
        Modules[Index]->As<Shape2DClass>()->Render(Length, Size, Ratio, Transform, Mirrored, Layout, Buffer);

    // Final output
    for (uint32_t Index = 0; Index < Length; Index++)
    {
        Buffer[Index].ToDisplay(Brightness);
        LEDs.Write(Index, Buffer[Index]);
    }

    return true;
};