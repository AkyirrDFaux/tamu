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

Program::Program(const Reference &ID, FlagClass Flags, RunInfo Info)
    : BaseClass(&Table, ID, Flags, Info)
{
    Type = ObjectTypes::Program;
    Name = "Program";

    uint16_t cursor = 0;

    // Index 0: Program Mode {0} (Depth 0)
    ProgramTypes mode = ProgramTypes::Sequence;
    Values.Set(&mode, sizeof(ProgramTypes), Types::Program, cursor++, 0);

    // Index 1: Counter {0, 0} (Depth 1)
    int32_t counter = 0;
    Values.Set(&counter, sizeof(int32_t), Types::Integer, cursor++, 1);
}

Program::~Program() {}

bool Program::RunEntry(uint16_t Index)
{
    // Retrieve the entry via NodeIdx (Group 1+)
    Result item = Values.Get(Index);

    // If no operation is found, we consider this entry "complete"
    if (item.Length == 0 || item.Type != Types::Operation)
        return true;
    
    // Operation::Run now takes the raw enum and the values tree
    return RunOperation({&Values, Index});
}

bool Program::Run()
{
    // 1. Direct access to Control Logic
    // Index 0: Mode {0}
    // Index 1: Counter {0, 0}

    Result modeRes = Values.Get(0);
    Result countRes = Values.Get(1);

    if (!modeRes.Value || !countRes.Value)
    {
        ReportError(Status::MissingModule);
        return true;
    }

    ProgramTypes Mode = *(ProgramTypes *)modeRes.Value;
    int32_t counter = *(int32_t *)countRes.Value; //Pointer can CHANGE during execution!!!
    int32_t InitialCounter = counter;

    bool HasFinished = false;

    // 2. Execution Logic
    if (Mode == ProgramTypes::Sequence)
    {
        // First executable entry is always Index 2 (The sibling after Branch {0})
        uint16_t entryIdx = 2;

        // Fast-forward to the current step (O(N) search, but starting from known Index 2)
        for (int32_t i = 0; i < counter && entryIdx != INVALID_HEADER; i++)
        {
            entryIdx = Values.Next(entryIdx);
        }

        // Execution Loop
        while (entryIdx != INVALID_HEADER)
        {
            if (counter >= InitialCounter && HasFinished)
                    break;

            if (!RunEntry(entryIdx))
                break; // Current entry is busy/blocking (e.g., a Wait command)

            counter++;
            entryIdx = Values.Next(entryIdx);

            // Wrap-around logic
            if (entryIdx == INVALID_HEADER)
            {
                counter = 0;
                entryIdx = 2; // Jump back to the first entry
                HasFinished = true;
            }
        }
    }
    else if (Mode == ProgramTypes::All)
    {
        HasFinished = true;
        // Start parallel execution from the first entry (Index 2)
        uint16_t entryIdx = 2;

        while (entryIdx != INVALID_HEADER)
        {
            if (!RunEntry(entryIdx))
                HasFinished = false; // At least one entry is still working

            entryIdx = Values.Next(entryIdx);
        }
    }
    Values.SetExisting(&counter, sizeof(int32_t), Types::Integer, 1);

    return HasFinished;
}