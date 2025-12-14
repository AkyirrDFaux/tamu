class LEDStripClass : public BaseClass
{
public:
    enum Value
    {
        LEDType,
        Length,
        Brightness,
    };
    enum Module
    {
        Port
    };

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

    Values.Add(LEDStrips::Undefined);
    Values.Add<int32_t>(0, Length);
    Values.Add<uint8_t>(20, Brightness);
};

LEDStripClass::~LEDStripClass()
{
    Outputs.Remove(this);
};

bool LEDStripClass::Run()
{
    PortClass *Port = Modules.Get<PortClass>(Module::Port); // HW connection
    LEDStrips *Type = Values.At<LEDStrips>(Value::LEDType);
    int32_t *Length = Values.At<int32_t>(Value::Length);
    uint8_t *Brightness = Values.At<uint8_t>(Value::Brightness);

    if (Type == nullptr || Port == nullptr || Length == nullptr || Brightness == nullptr)
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

    // Iterate over corrected pixel index
    for (int32_t Index = 0; Index < *Length; Index++)
    {
        ColourClass PixelColour = RenderPixel(Index, *Length);

        PixelColour.ToDisplay(*Brightness);
        Pixel[Index].setRGB(PixelColour.R, PixelColour.G, PixelColour.B);
    }
    return true;
};

ColourClass LEDStripClass::RenderPixel(int32_t Base, int32_t Length)
{
    ColourClass Colour = ColourClass(0, 0, 0);

    for (int32_t Index = Modules.FirstValid(ObjectTypes::LEDSegment,1); Index < Modules.Length; Modules.Iterate(&Index, ObjectTypes::LEDSegment))
        Colour = Modules[Index]->As<LEDSegmentClass>()->Render(Colour, Base, Length);

    return Colour;
};