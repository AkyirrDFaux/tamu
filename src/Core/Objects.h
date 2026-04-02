class FlagClass
{
public:
    uint8_t Values = Flags::None;
    FlagClass() {};
    FlagClass(uint8_t Input) { Values = Input; };
    void operator=(uint8_t Other) { Values = Other; };
    void operator+=(uint8_t Other) { Values |= Other; };
    void operator-=(uint8_t Other) { Values &= ~Other; };
    bool operator==(Flags Other) { return (Values & Other) != 0; };
    operator Flags() { return (Flags)Values; };
};

struct RunInfo
{
    uint8_t Period = 0;
    uint8_t Phase = 0;
};

struct RegisterEntry
{
    union
    {
        uint16_t ID;
        struct
        {
            uint8_t DeviceID;
            uint8_t GroupID;
        };
    };
    RunInfo Info;
    BaseClass *Object;
};

class RegisterClass
{
public:
    RegisterEntry *Object = nullptr;
    uint16_t Allocated = 0;
    uint16_t Registered = 0;

    RegisterClass() {};
    int32_t Search(const Reference &ID) const;
    int32_t Search(const BaseClass *SearchObject) const;
    Reference GetReference(const BaseClass *SearchObject) const;
    BaseClass *At(const Reference &ID) const;

    // Pass by value is fine for the 12-byte Reference in operators
    BaseClass *operator[](Reference ID) const { return At(ID); };

    bool IsValid(const Reference &ID, ObjectTypes Filter = ObjectTypes::Undefined) const;

    SearchResult Find(const Reference &Location, bool StopAtReferences = false) const;
    void ValueSetup(const void *Data, size_t Size, Types Type, const Reference &Location);

    void Expand(uint32_t NewAllocated);
    void Shorten();

    bool Register(BaseClass *AddObject, const Reference &ID, RunInfo Info = {0, 0});
    bool Unregister(int32_t Index);
} Objects;

struct VTable
{
    // Changed Path to Reference for consistent branch initialization
    void (*Setup)(BaseClass *self, const Reference &Index);
    bool (*Run)(BaseClass *self);
};

class BaseClass
{
public:
    const VTable *const Vptr;
    ObjectTypes Type = ObjectTypes::Undefined;
    FlagClass Flags = Flags::None;
    uint8_t RunPeriod = 0;
    uint8_t RunPhase = 0;
    Text Name = "";
    ByteArray Values;

    // Constructor now takes the new bit-tagged Reference
    BaseClass(const VTable *Table, const Reference &NewID, FlagClass Flags = Flags::None, RunInfo Info = {0, 0})
        : Vptr(Table), Flags(Flags), RunPeriod(Info.Period)
    {
        Objects.Register(this, NewID, Info);
    };

    ~BaseClass() { Objects.Unregister(Objects.Search(this)); };

    void Destroy();

    // Updated Setup to use Reference
    void Setup(const Reference &Index) { Vptr->Setup(this, Index); }
    RunInfo Run(FlagClass RunFlags)
    {
        if (Flags == Flags::Inactive)// || !(Flags == RunFlags)) // Should not run
            return {0, 0};

        if (Vptr->Run(this)) // Finished
        {
            if (Flags == Flags::RunOnce)
                Flags -= Flags::RunOnce;

            if (Flags == Flags::RunLoop)// && RunFlags == RunLoop)
                return {RunPeriod, RunPhase};
            else
                return {0, 0};
        }
        else // Not done, continue
            return {RunPeriod == 0 ? (uint8_t)1 : RunPeriod, RunPhase};
    }

    static void DefaultSetup(BaseClass *self, const Reference &Index) { /* Empty default */ };
    static bool DefaultRun(BaseClass *self)
    {
        // ReportError(Status::InvalidType);
        return true;
    };

    template <class C>
    C *As() const { return (C *)this; };
    void Save();

    SearchResult Find(const Reference &Location, bool StopAtReferences = false) const;
    void ValueSetupDirect(const void *Data, size_t Size, Types Type, const Bookmark &Point);
    void ValueSetup(const void *Data, size_t Size, Types Type, const Reference &Location);
    ByteArray Compress() const;
};