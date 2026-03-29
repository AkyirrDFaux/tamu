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

    // Pass by value is fine for the 12-byte Reference in operators
    BaseClass *operator[](Reference ID) const { return At(ID); };

    bool IsValid(const Reference &ID, ObjectTypes Filter = ObjectTypes::Undefined) const;

    SearchResult Find(const Reference &Location, bool StopAtReferences = false) const;
    void ValueSetup(const void *Data, size_t Size, Types Type, const Reference &Location);

    void Expand(uint32_t NewAllocated);
    void Shorten();

    bool Register(BaseClass *AddObject, const Reference &ID, ObjectInfo Info = {Flags::None, 0});
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
    Text Name = "";
    ByteArray Values;

    // Constructor now takes the new bit-tagged Reference
    BaseClass(const VTable *Table, const Reference &NewID, ObjectInfo Info = {Flags::None, 0})
        : Vptr(Table)
    {
        Objects.Register(this, NewID, Info);
    };

    ~BaseClass() { Objects.Unregister(Objects.Search(this)); };

    void Destroy();

    // Updated Setup to use Reference
    void Setup(const Reference &Index) { Vptr->Setup(this, Index); }
    bool Run() { return Vptr->Run(this); }

    static void DefaultSetup(BaseClass *self, const Reference &Index) { /* Empty default */ };
    static bool DefaultRun(BaseClass *self)
    {
        ReportError(Status::InvalidType);
        return true;
    };

    template <class C>
    C *As() const { return (C *)this; };
    void Save();

    SearchResult Find(const Bookmark &Parent, const Reference &RelativeLocation, bool StopAtReferences = false) const;
    void ValueSetupDirect(const void *Data, size_t Size, Types Type, const Bookmark &Point);
    void ValueSetup(const void *Data, size_t Size, Types Type, const Reference &Location);
    ByteArray Compress() const;
};