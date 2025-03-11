class Program : public Variable<ProgramTypes>
{
public:
    uint32_t Counter = 0;
    Program(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~Program();
    bool RunEntry(uint32_t Counter);
    bool Run();
};

Program::Program(bool New, IDClass ID, FlagClass Flags) : Variable(ProgramTypes::None, ID, Flags)
{
    Type = Types::Program;
    Name = "Program";
    Programs.Add(this);
}

Program::~Program()
{
    Programs.Remove(this);
}

bool Program::RunEntry(uint32_t Counter)
{
    if (Modules.IsValid(Counter) == false)
        return true;

    BaseClass *Object = Modules[Counter];

    return Object->Run();
}

bool Program::Run()
{
    bool HasFinished = true;
    int CounterInit = Counter;
    switch (*Data)
    {
    case ProgramTypes::Sequence:
        HasFinished = false;
        while (RunEntry(Counter) == true && !(HasFinished && Counter >= CounterInit))
        {
            Counter++;
            if (Counter >= Modules.Length)
            {
                HasFinished = true;
                Counter = 0;
            }
        }
        break;
    case ProgramTypes::All:
        for (int32_t Index = Modules.FirstValid(); Index < Modules.Length; Modules.Iterate(&Index))
            HasFinished = HasFinished && RunEntry(Counter);

        break;
    default:
        break;
    }

    if ((Flags == Flags::RunOnce) && HasFinished)
        Flags -= Flags::RunOnce;

    return HasFinished;
}
