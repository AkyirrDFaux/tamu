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

    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = BaseClass::DefaultRun,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};

    ColourClass Render(ColourClass Current, int32_t Position, int32_t Length);
};

constexpr VTable LEDSegmentClass::Table;

LEDSegmentClass::LEDSegmentClass(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags)
{
    Type = ObjectTypes::LEDSegment;
    Name = "LED Segment";

    ValueSet<int32_t>(0, Start);
    ValueSet<int32_t>(0, End);

    AddModule(new Texture1D(), Texture);
};

LEDSegmentClass::~LEDSegmentClass() {

};

ColourClass LEDSegmentClass::Render(ColourClass Current, int32_t Position, int32_t Length)
{
    if (Values.Type(Start) != Types::Integer || Values.Type(End) != Types::Integer || Modules.IsValid(Texture, ObjectTypes::Texture1D) == false)
        return Current;

    int32_t Start = ValueGet<int32_t>(Value::Start);
    int32_t End = ValueGet<int32_t>(Value::End);
    Texture1D *Texture = Modules.Get<Texture1D>(Module::Texture);

    if (End < Start) // Wrap around, not working yet
    {
        int32_t SegmentLength = Length - End + Start + 1;
        if (Position <= End)
            return Texture->Render(Position + End - SegmentLength / 2);
        if (Position >= Start)
            return Texture->Render(Position - Start - SegmentLength / 2);
    }
    else // Normal
    {
        if (Position >= Start && Position <= End)
            return Texture->Render(Position - Start - (End - Start) / 2); // Center to middle of segment
    }
    return Current;
};