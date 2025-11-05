class Program : public BaseClass
{
public:
    enum Value
    {
        Mode,
        Counter
    };
    Program(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~Program();
    bool RunEntry(uint32_t Counter);
    bool Run();
};

Program::Program(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags)
{
    Type = Types::Program;
    Name = "Program";

    Values.Add(ProgramTypes::None);
    Values.Add<uint32_t>(0);

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

    ProgramTypes *Mode = Values.At<ProgramTypes>(Value::Mode);
    uint32_t *Counter = Values.At<uint32_t>(Value::Counter);

    int CounterInit = *Counter;
    switch (*Mode)
    {
    case ProgramTypes::Sequence:
        HasFinished = false;
        while (RunEntry(*Counter) == true && !(HasFinished && *Counter >= CounterInit)) // Finished part or (made a step and done)
        {
            (*Counter)++;
            if (*Counter >= Modules.Length)
            {
                HasFinished = true;
                *Counter = 0;
            }
            if (HasFinished && Flags == Flags::RunOnce)
                break;
        }
        break;
    case ProgramTypes::All:
        //NOT WORKING
        for (int32_t Index = Modules.FirstValid(); Index < Modules.Length; Modules.Iterate(&Index))
            HasFinished = HasFinished && RunEntry(*Counter);
        break;
    default:
        break;
    }

    if ((Flags == Flags::RunOnce) && HasFinished)
    {
        Flags -= Flags::RunOnce;
        Chirp.Send(ByteArray(Functions::SetFlags) << ID << Flags);
    }

    return HasFinished;
}
