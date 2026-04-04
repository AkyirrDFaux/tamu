class Program : public BaseClass
{
public:
    Program(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});
    ~Program();

    bool RunEntry(uint16_t Index);
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
    Values.Set(&mode, sizeof(ProgramTypes), Types::Program, Reference({0}));

    // Set Counter at {0, 0}
    int32_t counter = 0;
    Values.Set(&counter, sizeof(int32_t), Types::Integer, Reference({0, 0}));
}

Program::~Program() {}

bool Program::RunEntry(uint16_t Index)
{
    // Retrieve the entry via NodeIdx (Group 1+)
    Result item = Values.Get(Index);

    // If no operation is found, we consider this entry "complete"
    if (item.Length == 0 || item.Type != Types::Operation)
        return true;

    // Direct pointer cast to the Operations enum
    Operations op = *(Operations *)item.Value;

    // Operation::Run now takes the raw enum and the values tree
    return Operation::Run(op, {&Values,Index});
}

bool Program::Run()
{
    // 1. Fetch Mode {0} and Counter {0, 0} via Bookmarks
    Bookmark modeMark = Values.Find({0}, true);
    Bookmark countMark = Values.Find({0, 0}, true);

    Result modeRes = Values.Get(modeMark);
    Result countRes = Values.Get(countMark);

    if (modeRes.Length == 0 || countRes.Length == 0)
        return true;

    ProgramTypes Mode = *(ProgramTypes *)modeRes.Value;
    int32_t Counter = *(int32_t *)countRes.Value;
    int32_t InitialCounter = Counter;

    bool HasFinished = false;

    // Retrieve the root of the executable entries (Branch {1} and above)
    // We assume executable logic starts at the first child after the control branch {0}
    uint16_t entryIdx = 0;

    if (Mode == ProgramTypes::Sequence)
    {
        // Skip branch {0} (Control) to get to executable steps
        entryIdx = Values.Next(entryIdx);
        // Advance to the current step indicated by Counter
        for (int32_t i = 0; i < Counter && entryIdx != 0xFFFF; i++)
        {
            entryIdx = Values.Next(entryIdx);
        }

        // Run until an entry returns 'false' (it's busy) or we wrap
        while (entryIdx != 0xFFFF && RunEntry(entryIdx))
        {
            Counter++;
            entryIdx = Values.Next(entryIdx);

            // If we hit the end of the list, wrap back to the first executable entry
            if (entryIdx == 0xFFFF)
            {
                Counter = 0;
                entryIdx = Values.Next(0); // Reset to first step (skip {0})
                HasFinished = true;

                // Safety: break if we've cycled back to where we started in one tick
                if (Counter == InitialCounter)
                    break;
            }
        }
    }
    else if (Mode == ProgramTypes::All)
    {
        HasFinished = true;

        // Start at the first executable entry (Skip control branch {0})
        entryIdx = Values.Next(Values.Child(0xFFFF));

        while (entryIdx != 0xFFFF)
        {
            // If any entry returns false (busy), the whole program isn't "finished"
            if (!RunEntry(entryIdx))
            {
                HasFinished = false;
            }
            entryIdx = Values.Next(entryIdx);
        }
    }

    // 2. Save the updated logical Counter back to {0, 0}
    Values.Set(&Counter, sizeof(int32_t), Types::Integer, Reference({0, 0}));

    return HasFinished;
}