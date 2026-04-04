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

    // 1. Root {0}: Program Mode
    ProgramTypes mode = ProgramTypes::Sequence;
    Values.Set(&mode, sizeof(ProgramTypes), Types::Program, 0);

    // 2. Child of Mode {0, 0}: Counter
    int32_t counter = 0;
    Values.InsertChild(&counter, sizeof(int32_t), Types::Integer, 0);
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
    // 1. Resolve Control Logic via direct indexing
    uint16_t modeIdx = 0;
    uint16_t countIdx = Values.Child(modeIdx);

    Result modeRes = Values.Get(modeIdx);
    Result countRes = Values.Get(countIdx);

    if (!modeRes.Value || !countRes.Value)
        return true;

    ProgramTypes Mode = *(ProgramTypes *)modeRes.Value;
    int32_t* counterPtr = (int32_t *)countRes.Value; // Get pointer to update directly
    int32_t InitialCounter = *counterPtr;

    bool HasFinished = false;

    // 2. Execution Logic
    if (Mode == ProgramTypes::Sequence)
    {
        // First executable entry is the sibling after the Mode branch {0}
        uint16_t entryIdx = Values.Next(modeIdx);

        // Fast-forward to the current step (Counter)
        for (int32_t i = 0; i < *counterPtr && entryIdx != 0xFFFF; i++)
        {
            entryIdx = Values.Next(entryIdx);
        }

        // Execution Loop
        while (entryIdx != 0xFFFF)
        {
            if (!RunEntry(entryIdx)) 
                break; // Current entry is busy/blocking

            (*counterPtr)++;
            entryIdx = Values.Next(entryIdx);

            // Wrap-around logic
            if (entryIdx == 0xFFFF)
            {
                *counterPtr = 0;
                entryIdx = Values.Next(modeIdx);
                HasFinished = true;

                // Safety: Avoid infinite loop if all entries return true instantly
                if (*counterPtr == InitialCounter) break;
            }
        }
    }
    else if (Mode == ProgramTypes::All)
    {
        HasFinished = true;
        // Run every sibling after the Mode branch
        uint16_t entryIdx = Values.Next(modeIdx);

        while (entryIdx != 0xFFFF)
        {
            if (!RunEntry(entryIdx))
                HasFinished = false;
            
            entryIdx = Values.Next(entryIdx);
        }
    }

    return HasFinished;
}