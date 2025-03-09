class LEDSegmentClass : public Variable<uint8_t>
{
public:
    enum Module
    {
        Start,
        End,
        Texture,
    };

    LEDSegmentClass(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~LEDSegmentClass();

    ColourClass Render(ColourClass Current, int32_t Position, int32_t Length);
};

LEDSegmentClass::LEDSegmentClass(bool New, IDClass ID, FlagClass Flags) : Variable(0, ID, Flags)
{
    Type = Types::LEDSegment;
    Name = "LED Segment";
    if (New)
    {
        AddModule(new Variable<int32_t>(0), Start);
        Modules[Module::Start]->Name = "Start";
        AddModule(new Variable<int32_t>(0), End);
        Modules[Module::End]->Name = "End";
        AddModule(new Texture1D(), Texture);
    }
};

LEDSegmentClass::~LEDSegmentClass() {

};

ColourClass LEDSegmentClass::Render(ColourClass Current, int32_t Position, int32_t Length)
{
    int32_t *Start = Modules.GetValue<int32_t>(Module::Start);
    int32_t *End = Modules.GetValue<int32_t>(Module::End);
    Texture1D *Texture = Modules.Get<Texture1D>(Module::Texture);

    if (Texture == nullptr || Start == nullptr || End == nullptr)
        return Current;

    if (*End < *Start) // Wrap around, not working yet
    {
        int32_t SegmentLength = Length - *End + *Start + 1;
        if (Position <= *End)
            return Texture->Render(Position + *End - SegmentLength/2);
        if (Position >= *Start)
            return Texture->Render(Position - *Start - SegmentLength/2);
    }
    else // Normal
    {
        if (Position >= *Start && Position <= *End)
            return Texture->Render(Position - *Start - (*End - *Start) / 2); //Center to middle of segment
    }
    return Current;
};