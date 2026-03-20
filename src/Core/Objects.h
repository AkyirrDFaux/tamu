struct RegisterEntry
{
    BaseClass *Object;
    uint16_t Index;
};

class RegisterClass
{
public:
    RegisterEntry *Object = nullptr;
    uint16_t Allocated = 0;
    uint16_t Registered = 0;

    RegisterClass() {};

    int32_t Search(const Reference &ID) const;
    int32_t Search(BaseClass *SearchObject) const;
    BaseClass *At(const Reference &ID) const;
    BaseClass *operator[](Reference ID) const { return At(ID); };
    bool IsValid(const Reference &ID, ObjectTypes Filter = ObjectTypes::Undefined) const;

    template <class C>
    C *ValueGet(const Reference &ID) const;
    template <class C>
    bool ValueSet(C Value, const Reference &ID);
    Types ValueTypeAt(const Reference &ID) const;

    void Expand(uint32_t NewAllocated);
    void Shorten();

    bool Register(BaseClass *AddObject, const Reference &ID);
    bool Unregister(int32_t Index);
} Objects;

class FlagClass
{
public:
    uint8_t Values = Flags::None;
    FlagClass() {};
    FlagClass(uint8_t Input) { Values = Input; };
    void operator=(uint8_t Other) { Values = Other; };
    void operator+=(uint8_t Other) { Values |= Other; };
    void operator-=(uint8_t Other) { Values &= ~Other; };
    bool operator==(Flags Other) { return Values & Other; };
};

struct VTable
{
    void (*Setup)(BaseClass *self, Path Index);
    bool (*Run)(BaseClass *self);
};

class BaseClass
{
public:
    const VTable *const Vptr;
    ObjectTypes Type = ObjectTypes::Undefined;
    FlagClass Flags;
    Text Name = "";
    ByteArray Values;

    BaseClass(const VTable *Table, Reference NewID, FlagClass NewFlags = Flags::None)
        : Vptr(Table), Flags(NewFlags)
    {
        Objects.Register(this, NewID);
    };

    ~BaseClass() { Objects.Unregister(Objects.Search(this)); };
    void Destroy();
    void Setup(Path Index) { Vptr->Setup(this, Index); }
    bool Run() { return Vptr->Run(this); }

    static void DefaultSetup(BaseClass *self, Path Index) { /* Empty default */ };
    static bool DefaultRun(BaseClass *self)
    {
        ReportError(Status::InvalidType);
        return true;
    };

    template <class C>
    C *As() const { return (C *)this; };
    void Save();

    template <class C>
    Getter<C> ValueGet(const Path &Location) const;
    template <class C>
    bool ValueSet(C Value, const Path &Location);

    // ByteArray OutputValues(int32_t Value = 0) const;
    // bool InputValues(ByteArray &Input, int32_t Index, uint8_t Value = 0);
};
