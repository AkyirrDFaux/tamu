class LEDStripClass : public Variable<LEDStrips>
{
public:
    enum Module
    {
        PortAttach,
        Length,
        Brightness,
        Parts
    };

    LEDStripClass(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~LEDStripClass();

    bool Run();
    ColourClass RenderPixel(int32_t Base, int32_t Length);
};

LEDStripClass::LEDStripClass(bool New, IDClass ID, FlagClass Flags) : Variable(LEDStrips::Undefined, ID, Flags)
{
    BaseClass::Type = Types::LEDStrip;
    Name = "LED Strip";
    Outputs.Add(this);

    if (New)
    {
        AddModule(new Variable<int32_t>(0), Length);
        Modules[Length]->Name = "Length";
        AddModule(new PortAttachClass(New), PortAttach);
        AddModule(new Variable<uint8_t>(20), Brightness);
        Modules[Brightness]->Name = "Brightness";
        AddModule(new Folder(true), Parts);
        Modules[Parts]->Name = "Parts";
    }
};

LEDStripClass::~LEDStripClass()
{
    Outputs.Remove(this);
};

bool LEDStripClass::Run()
{
    PortAttachClass *DisplayPort = Modules.Get<PortAttachClass>(Module::PortAttach); // HW connection
    int32_t *Length = Modules.GetValue<int32_t>(Module::Length);
    uint8_t *Brightness = Modules.GetValue<uint8_t>(Module::Brightness);

    if (DisplayPort == nullptr || Length == nullptr || Brightness == nullptr)
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

    // Iterate over corrected pixel coords |_
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
    Folder *Parts = Modules.Get<Folder>(Module::Parts);

    ColourClass Colour = ColourClass(0, 0, 0);
    if (Parts == nullptr)
    {
        ReportError(Status::MissingModule);
        return Colour;
    }
    
    for (int32_t Index = Parts->Modules.FirstValid(Types::LEDSegment); Index < Parts->Modules.Length; Parts->Modules.Iterate(&Index, Types::LEDSegment))
        Colour = Parts->Modules[Index]->As<LEDSegmentClass>()->Render(Colour, Base, Length);

    return Colour;
};