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
};

struct ObjectInfo
{
    FlagClass Flags = Flags::None;
    uint8_t RunTiming = 0;
};

struct RegisterEntry
{
    BaseClass *Object;
    union
    {
        uint16_t ID;
        struct
        {
            uint8_t DeviceID;
            uint8_t GroupID;
        };
    };
    ObjectInfo Info;
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
    BaseClass *operator[](Reference ID) const { return At(ID); };
    bool IsValid(const Reference &ID, ObjectTypes Filter = ObjectTypes::Undefined) const;

    template <class C>
    C *ValueGet(const Reference &ID) const;
    template <class C>
    bool ValueSet(C Value, const Reference &ID);
    Types ValueTypeAt(const Reference &ID) const;

    void Expand(uint32_t NewAllocated);
    void Shorten();

    bool Register(BaseClass *AddObject, const Reference &ID, ObjectInfo Info = {Flags::None, 0});
    bool Unregister(int32_t Index);
} Objects;

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
    Text Name = "";
    ByteArray Values;

    BaseClass(const VTable *Table, const Reference &NewID, ObjectInfo Info = {Flags::None, 0})
        : Vptr(Table)
    {
        Objects.Register(this, NewID, Info);
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
    bool ValueSetup(C Value, const Path &Location);
};
