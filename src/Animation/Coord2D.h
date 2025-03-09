class AnimationCoord : public Variable<CoordAnimations>
{
public:
    AnimationCoord(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~AnimationCoord();
    void Setup();
    bool Run();

    enum Module
    {
        Time,
        TargetA,
        TargetB,
        Phase
    };
};

AnimationCoord::AnimationCoord(bool New, IDClass ID, FlagClass Flags) : Variable(CoordAnimations::None, ID, Flags)
{
    BaseClass::Type = Types::AnimationCoord;
    Name = "Coord Animation";
};

AnimationCoord::~AnimationCoord()
{
    Routines.Remove(this);
}

void AnimationCoord::Setup()
{
    switch (*Data)
    {
    case CoordAnimations::MoveTo:
        if (!Modules.IsValidID(Time))
        {
            AddModule(new Variable<uint32_t>(0), Module::Time);
            Modules[Time]->Name = "Time";
        }
        if (!Modules.IsValidID(TargetA))
        {
            AddModule(new Variable<Coord2D>(Coord2D(0, 0, 0)), Module::TargetA);
            Modules[TargetA]->Name = "Target";
        }
        break;
    }
    // Other cases like loops maybe use, Routines.Add(this);
};

bool AnimationCoord::Run()
{
    uint32_t *Time = Modules.GetValue<uint32_t>(Module::Time);
    Coord2D *TargetA = Modules.GetValue<Coord2D>(Module::TargetA);

    switch (*Data)
    {
    case CoordAnimations::MoveTo:
        if (TargetA == nullptr || Time == nullptr)
            return true;
        //"2" might be wrong
        for (int32_t Index = Modules.FirstValid(Types::Coord2D,2); Index < Modules.Length; Modules.Iterate(&Index, Types::Coord2D))
        {
            Coord2D *Value = Modules.GetValue<Coord2D>(Index);
            *Value = Value->TimeMove(*TargetA, *Time);
        }
        return (CurrentTime >= *Time);
    }
    return true;
};