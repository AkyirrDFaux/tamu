class Program : public BaseClass
{
public:
    Program(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});
    ~Program();

    bool RunEntry(uint8_t Index);
    bool Run();

    static bool RunBridge(BaseClass *Base) { return static_cast<Program *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = Program::RunBridge};
};

Program::Program(const Reference &ID, FlagClass Flags, RunInfo Info) : BaseClass(&Table, ID, Flags, Info)
{
    Type = ObjectTypes::Program;
    Name = "Program";

    // Set Program Mode at {0}
    ProgramTypes mode = ProgramTypes::Sequence;
    Values.Set(&mode, sizeof(ProgramTypes), Types::Program, {0});

    // Set Counter at {0, 0}
    int32_t counter = 0;
    Values.Set(&counter, sizeof(int32_t), Types::Integer, {0, 0});
}

Program::~Program() {}

bool Program::RunEntry(uint8_t Index)
{
    // Retrieve the entry at Path {Index} (Group 1+)
    SearchResult item = Values.Find({Index});

    // If no operation is found, we consider this entry "complete"
    if (item.Length == 0 || item.Type != Types::Operation)
        return true;

    // Direct pointer cast to the Operations enum
    Operations op = *(Operations *)item.Value;

    // Operation::Run now takes the raw enum and the values tree
    return Operation::Run(op, Values, this, {Index});
}

bool Program::Run()
{
    // 1. Fetch Mode {0} and Counter {0, 0}
    SearchResult modeRes = Values.Find({0}, true);
    SearchResult countRes = Values.Find({0, 0}, true);

    if (modeRes.Length == 0 || countRes.Length == 0)
        return true;

    ProgramTypes Mode = *(ProgramTypes *)modeRes.Value;
    int32_t Counter = *(int32_t *)countRes.Value;
    int32_t InitialCounter = Counter;

    bool HasFinished = false;

    if (Mode == ProgramTypes::Sequence)
    {
        // Run until an entry returns 'false' (it's busy) or we loop back
        // The Counter maps to the Path {Counter + 1}
        while (RunEntry((uint8_t)(Counter + 1)))
        {
            Counter++;

            // Check if next path exists; if not, wrap around to start of sequence
            SearchResult next = Values.Find({(uint8_t)(Counter + 1)});
            if (next.Length == 0)
            {
                Counter = 0;
                HasFinished = true;

                // If the object info says RunOnce, we stop the loop here
                // if (Info.Flags == Flags::RunOnce)
                //    break;
            }

            // Safety break to prevent infinite loops in a single execution tick
            if (Counter == InitialCounter)
                break;
        }
    }
    else if (Mode == ProgramTypes::All)
    {
        HasFinished = true;
        // Iterate through all top-level indices (1 to 255)
        for (uint8_t i = 1; i < 255; i++)
        {
            SearchResult entry = Values.Find({i});
            if (entry.Length == 0)
                break; // End of program data

            // If any entry returns false (busy), the whole program isn't "finished"
            if (!RunEntry(i))
                HasFinished = false;
        }
    }

    // 2. Save the updated logical Counter back to {0, 0}
    // Anchor the counter variable for the pointer-based Set
    int32_t updatedCounter = Counter;
    Values.Set(&updatedCounter, sizeof(int32_t), Types::Integer, {0, 0});

    return HasFinished;
}