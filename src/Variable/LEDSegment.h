class LEDSegmentClass : public BaseClass
{
public:
    enum Value
    {
        Start,
        End
    };
    enum Module
    {
        Texture
    };

    LEDSegmentClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~LEDSegmentClass();

    ColourClass Render(ColourClass Current, int32_t Position, int32_t Length);
};

LEDSegmentClass::LEDSegmentClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    Type = Types::LEDSegment;
    Name = "LED Segment";

    Values.Add<int32_t>(0, Start);
    Values.Add<int32_t>(0, End);

    AddModule(new Texture1D(), Texture);

};

LEDSegmentClass::~LEDSegmentClass() {

};

ColourClass LEDSegmentClass::Render(ColourClass Current, int32_t Position, int32_t Length)
{
    int32_t *Start = Values.At<int32_t>(Value::Start);
    int32_t *End = Values.At<int32_t>(Value::End);
    Texture1D *Texture = Modules.Get<Texture1D>(Module::Texture);

    if (Texture == nullptr || Start == nullptr || End == nullptr)
        return Current;

    if (*End < *Start) // Wrap around, not working yet
    {
        int32_t SegmentLength = Length - *End + *Start + 1;
        if (Position <= *End)
            return Texture->Render(Position + *End - SegmentLength / 2);
        if (Position >= *Start)
            return Texture->Render(Position - *Start - SegmentLength / 2);
    }
    else // Normal
    {
        if (Position >= *Start && Position <= *End)
            return Texture->Render(Position - *Start - (*End - *Start) / 2); // Center to middle of segment
    }
    return Current;
};