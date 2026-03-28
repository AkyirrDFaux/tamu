class Program : public BaseClass
{
public:
    Program(const Reference &ID, ObjectInfo Info = {Flags::None, 1});
    ~Program();

    bool RunEntry(uint8_t Index);
    bool Run();

    static bool RunBridge(BaseClass *Base) { return static_cast<Program *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = Program::RunBridge};
};

Program::Program(const Reference &ID, ObjectInfo Info) : BaseClass(&Table, ID, Info)
{
    Type = ObjectTypes::Program;
    Name = "Program";

    // Set Program Mode at {0}
    Values.Set(ProgramTypes::Sequence, {0});
    // Set Counter at {0, 0}
    Values.Set((int32_t)0, {0, 0});
}

Program::~Program() {}

bool Program::RunEntry(uint8_t Index)
{
    // Retrieve the entry at Path {Index} (Group 1+)
    Getter<Operations> Item = Values.Get<Operations>({Index});
    if (!Item.Success)
        return true;

    return RunOperation(Item.Value, Values, {Index});
}

bool Program::Run()
{
    // 1. Fetch Mode {0} and Counter {0, 0}
    Getter<ProgramTypes> ModeEntry = Values.Get<ProgramTypes>({0});
    Getter<int32_t> CounterEntry = Values.Get<int32_t>({0, 0});

    if (!ModeEntry.Success || !CounterEntry.Success)
        return true;

    ProgramTypes Mode = ModeEntry.Value;
    int32_t Counter = CounterEntry.Value;
    int32_t InitialCounter = Counter;

    bool HasFinished = false;

    if (Mode == ProgramTypes::Sequence)
    {
        // Run until an entry returns 'false' (it's busy) or we loop back
        // The Counter maps to the Path {Counter + 1}
        while (RunEntry(Counter + 1))
        {
            Counter++;

            // Check if next path exists; if not, wrap around
            if (!Values.Get<Operations>({(uint8_t)(Counter + 1)}).Success)
            {
                Counter = 0;
                HasFinished = true;
                /*if (info.flags.has(Flags::runOnce))
                    break;*/
            }

            // Safety break to prevent infinite loops in one tick
            if (Counter == InitialCounter)
                break;
        }
    }
    /*else if (Mode == ProgramTypes::All)
    {
        HasFinished = true;

        if (Values.Get({i}).Success)
        {
            if (!RunEntry(i))
                HasFinished = false;
        }
    }*/

    // 2. Save the updated logical Counter back to {0, 0}
    Values.Set(Counter, {0, 0});

    return HasFinished;
}