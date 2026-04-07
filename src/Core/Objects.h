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

    Bookmark Find(const Reference &Location, bool StopAtReferences = false) const;
    //void ValueSetup(const void *Data, size_t Size, Types Type, const Reference &Location);

    void Expand(uint32_t NewAllocated);
    void Shorten();

    bool Register(BaseClass *AddObject, const Reference &ID, RunInfo Info = {0, 0});
    bool Unregister(int32_t Index);
} Objects;

struct VTable
{
    // Changed Path to Reference for consistent branch initialization
    void (*Setup)(BaseClass *self, uint16_t Index);
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
    ValueTree Values;

    // Constructor now takes the new bit-tagged Reference
    BaseClass(const VTable *Table, const Reference &NewID, FlagClass Flags = Flags::None, RunInfo Info = {0, 0})
        : Vptr(Table), Flags(Flags), RunPeriod(Info.Period), Values(this)
    {
        Objects.Register(this, NewID, Info);
    };

    ~BaseClass();

    void Destroy();

    // Updated Setup to use Reference
    void Setup(uint16_t Index) { Vptr->Setup(this, Index); }
    RunInfo Run(FlagClass RunFlags)
    {
        if (Flags == Flags::Inactive) // || !(Flags == RunFlags)) // Should not run
            return {0, 0};

        if (Vptr->Run(this)) // Finished
        {
            if (Flags == Flags::RunOnce)
                Flags -= Flags::RunOnce;

            if (Flags == Flags::RunLoop) // && RunFlags == RunLoop)
                return {RunPeriod, RunPhase};
            else
                return {0, 0};
        }
        else // Not done, continue
            return {RunPeriod == 0 ? (uint8_t)1 : RunPeriod, RunPhase};
    }

    static void DefaultSetup(BaseClass *self, uint16_t Index) { /* Empty default */ };
    static bool DefaultRun(BaseClass *self)
    {
        // ReportError(Status::InvalidType);
        return true;
    };

    template <class C>
    C *As() const { return (C *)this; };

    Bookmark Find(const Reference &Location, bool StopAtReferences = false) const;
    FlexArray Compress() const;
};

BaseClass *CreateObject(Reference ID, ObjectTypes Type);

FlexArray BaseClass::Compress() const
{
    // 1. Get the Reference struct/object
    Reference SelfRef = Objects.GetReference(this);

    // 2. Start the FlexArray with exactly 3 bytes from the Reference data
    // This assumes Reference.Data is the internal pointer to [Net, Group, Device]
    FlexArray Blob((const char *)SelfRef.Data, 3);

    // 3. Append ObjectType + Flags + Info (4 byte consecutive)
    Blob += FlexArray((char *)&Type, 4);

    // 5. Append Name (1-byte length prefix + data)
    uint8_t nameLen = (Name.Length > 255) ? 255 : (uint8_t)Name.Length - 1;
    Blob += FlexArray((char *)&nameLen, 1);
    if (nameLen > 0)
    {
        Blob += FlexArray(Name.Data, nameLen);
    }

    // 6. Serialize and Append Values
    // This calls the loop-based serialization we discussed
    Blob += Values.Serialize();

    return Blob;
}

Bookmark BaseClass::Find(const Reference &Location, bool StopAtReferences) const
{
    return Values.Find(Location, StopAtReferences);
}

void ValueTree::TriggerSetup(uint16_t index)
{
    if (Context != nullptr)
        Context->Setup(index);
}