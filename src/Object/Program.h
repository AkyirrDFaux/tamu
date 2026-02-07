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
    bool RunEntry(int32_t Counter);
    bool Run();

    static bool RunBridge(BaseClass *Base) { return static_cast<Program *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = Program::RunBridge,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};
};

constexpr VTable Program::Table;

Program::Program(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags)
{
    Type = ObjectTypes::Program;
    Name = "Program";

    ValueSet(ProgramTypes::None);
    ValueSet<int32_t>(0);

    Programs.Add(this);
}

Program::~Program()
{
    Programs.Remove(this);
}

bool Program::RunEntry(int32_t Counter)
{
    if (Modules.IsValid(Counter) == false)
        return true;

    BaseClass *Object = Modules[Counter];

    return Object->Run();
}

bool Program::Run()
{
    bool HasFinished = true;

    if (Values.Type(Mode) != Types::Program || Values.Type(Counter) != Types::Integer)
        return true;

    int32_t Counter = ValueGet<int32_t>(Value::Counter);

    int32_t CounterInit = Counter;
    switch (ValueGet<ProgramTypes>(Value::Mode))
    {
    case ProgramTypes::Sequence:
        HasFinished = false;
        while (RunEntry(Counter) == true && !(HasFinished && Counter >= CounterInit)) // Finished part or (made a step and done)
        {
            Counter++;
            if ((uint32_t)Counter >= Modules.Length)
            {
                HasFinished = true;
                Counter = 0;
            }
            if (HasFinished && Flags == Flags::RunOnce)
                break;
        }
        break;
    case ProgramTypes::All:
        // NOT WORKING
        for (uint32_t Index = Modules.FirstValid(); Index < Modules.Length; Modules.Iterate(&Index))
            HasFinished = HasFinished && RunEntry(Counter);
        break;
    default:
        break;
    }

    if ((Flags == Flags::RunOnce) && HasFinished)
    {
        Flags -= Flags::RunOnce;
        Chirp.Send(ByteArray(Functions::SetFlags) << ID << Flags);
    }

    ValueSet(Counter, Value::Counter);

    return HasFinished;
}
