class AnimationFloat : public Variable<FloatAnimations>
{
public:
    AnimationFloat(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~AnimationFloat();
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

AnimationFloat::AnimationFloat(bool New, IDClass ID, FlagClass Flags) : Variable(FloatAnimations::None, ID, Flags)
{
    BaseClass::Type = Types::AnimationFloat;
    Name = "Float Animation";
};

AnimationFloat::~AnimationFloat()
{
    Routines.Remove(this);
}

void AnimationFloat::Setup()
{
    switch (*Data)
    {
    case FloatAnimations::MoveTo:
        if (!Modules.IsValidID(Time))
        {
            AddModule(new Variable<uint32_t>(0), Module::Time);
            Modules[Time]->Name = "Time";
        }
        if (!Modules.IsValidID(TargetA))
        {
            AddModule(new Variable<float>(0), Module::TargetA);
            Modules[TargetA]->Name = "Target";
        }
        break;
    }
    // Other cases like loops maybe use, Routines.Add(this);
};

bool AnimationFloat::Run()
{
    uint32_t *Time = Modules.GetValue<uint32_t>(Module::Time);
    float *TargetA = Modules.GetValue<float>(Module::TargetA);

    switch (*Data)
    {
    case FloatAnimations::MoveTo:
        if (TargetA == nullptr || Time == nullptr)
            return true;
        //Following might be wrong ("2")
        for (int32_t Index = Modules.FirstValid(Types::Number, 2); Index < Modules.Length; Modules.Iterate(&Index, Types::Number))
        {
            float *Value = Modules.GetValue<float>(Index);
            *Value = TimeMove(*Value, *TargetA, *Time);
        }
        return (CurrentTime >= *Time);
    }
    return true;
};