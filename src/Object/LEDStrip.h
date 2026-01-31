class LEDStripClass : public BaseClass
{
public:
    enum Value
    {
        LEDType,
        Length,
        Brightness,
    };
    LEDDriver LEDs;

    LEDStripClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~LEDStripClass();

    bool Run();
    ColourClass RenderPixel(int32_t Base, int32_t Length);
};

LEDStripClass::LEDStripClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    BaseClass::Type = ObjectTypes::LEDStrip;
    Name = "LED Strip";
    Outputs.Add(this);

    ValueSet(LEDStrips::Undefined);
    ValueSet<int32_t>(0, Length);
    ValueSet<uint8_t>(20, Brightness);
};

LEDStripClass::~LEDStripClass()
{
    Outputs.Remove(this);
};

bool LEDStripClass::Run()
{
    if (Values.Type(Length) != Types::Integer || Values.Type(Brightness) != Types::Byte)
    {
        ReportError(Status::MissingModule, "LED Strip");
        return true;
    }

    if (LEDs.LEDs == nullptr)
    {
        ReportError(Status::PortError, "LED Strip");
        return true;
    }

    int32_t Length = ValueGet<int32_t>(Value::Length);
    uint8_t Brightness = ValueGet<uint8_t>(Value::Brightness);

    // Iterate over corrected pixel index
    for (int32_t Index = 0; Index < Length; Index++)
    {
        ColourClass PixelColour = RenderPixel(Index, Length);

        PixelColour.ToDisplay(Brightness);
        LEDs.Write(Index,PixelColour);
    }
    return true;
};

ColourClass LEDStripClass::RenderPixel(int32_t Base, int32_t Length)
{
    ColourClass Colour = ColourClass(0, 0, 0);

    for (int32_t Index = Modules.FirstValid(ObjectTypes::LEDSegment); Index < Modules.Length; Modules.Iterate(&Index, ObjectTypes::LEDSegment))
        Colour = Modules[Index]->As<LEDSegmentClass>()->Render(Colour, Base, Length);

    return Colour;
};