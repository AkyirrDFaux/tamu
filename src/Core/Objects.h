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

    int32_t Search(Reference ID) const;
    int32_t Search(BaseClass *SearchObject) const;
    BaseClass *At(Reference ID) const;
    BaseClass *operator[](Reference ID) const { return At(ID); };
    bool IsValid(Reference ID, ObjectTypes Filter = ObjectTypes::Undefined) const;

    template <class C>
    C *ValueGet(Reference ID) const;
    template <class C>
    bool ValueSet(C Value, Reference ID);
    Types ValueTypeAt(Reference ID) const;

    void Expand(uint32_t NewAllocated);
    void Shorten();

    bool Register(BaseClass *AddObject, Reference ID);
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
    void (*Setup)(BaseClass *self, int32_t Index);
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

    BaseClass(const VTable *Table, Reference NewID = {0, nullptr}, FlagClass NewFlags = Flags::None)
        : Vptr(Table), Flags(NewFlags)
    {
        Objects.Register(this, NewID);
    };

    ~BaseClass();
    void Destroy();
    void Setup(int32_t Index = -1) { Vptr->Setup(this, Index); }
    bool Run() { return Vptr->Run(this); }

    static void DefaultSetup(BaseClass *self, int32_t Index) { /* Empty default */ };
    static bool DefaultRun(BaseClass *self)
    {
        ReportError(Status::InvalidType);
        return true;
    };

    template <class C>
    C *As() const { return (C *)this; };
    void Save();

    template <class C>
    const C *ValueGet(const Reference &Reference) const;
    template <class C>
    bool ValueSet(C Value, const Reference &Reference);

    // ByteArray OutputValues(int32_t Value = 0) const;
    // bool InputValues(ByteArray &Input, int32_t Index, uint8_t Value = 0);
};
