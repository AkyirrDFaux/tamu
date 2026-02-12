class RegisterClass
{
public:
    BaseClass **Object = nullptr;
    uint32_t Allocated = 0;
    uint32_t Registered = 0;

    RegisterClass() {};

    BaseClass *At(IDClass ID) const;
    BaseClass *operator[](IDClass ID) const { return At(ID); };
    bool IsValid(IDClass ID, ObjectTypes Filter = ObjectTypes::Undefined) const;

    template <class C>
    C ValueGet(IDClass ID) const;
    template <class C>
    bool ValueSet(C Value, IDClass ID);
    Types ValueTypeAt(IDClass ID) const;

    void Expand(uint32_t NewAllocated);
    void Shorten();

    bool Register(BaseClass *AddObject, IDClass ID = RandomID);
    bool Unregister(IDClass ID);
    bool Unregister(BaseClass *RemovedObject);
} Objects;

class IDList
{
public:
    IDClass *IDs = nullptr;
    uint32_t Length = 0;

    ~IDList() { RemoveAll(); };

    IDList() {};
    IDList(const IDList &Copied);
    void operator=(const IDList &Copied);

    bool IsValid(int32_t Index, ObjectTypes Filter = ObjectTypes::Undefined) const;
    BaseClass *At(int32_t Index) const;
    BaseClass *operator[](int32_t Index) const { return At(Index); };
    ObjectTypes TypeAt(int32_t Index) const;
    int32_t FirstValid(ObjectTypes Filter = ObjectTypes::Undefined, int32_t Start = 0) const;
    void Iterate(uint32_t *Index, ObjectTypes Filter = ObjectTypes::Undefined) const;

    Types ValueTypeAt(int32_t Index) const;
    template <class C>
    C ValueGet(int32_t Index) const;
    template <class C>
    bool ValueSet(C Value, int32_t Index);

    void Expand(uint8_t NewLength);
    void Shorten();

    bool Add(IDClass ID, int32_t Index = -1);
    bool Add(BaseClass *AddObject, int32_t Index = -1);
    bool Remove(int32_t Index);
    bool Remove(BaseClass *RemovedObject);
    bool RemoveAll();

    template <class C>
    C *Get(int32_t Index);

    //String AsString();
};

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
    void (*AddModule)(BaseClass *self, BaseClass *Object, int32_t Index);
    void (*RemoveModule)(BaseClass *self, BaseClass *Object);
};

class BaseClass
{
public:
    const VTable *const Vptr;
    ObjectTypes Type = ObjectTypes::Undefined;
    IDClass ID;
    FlagClass Flags;
    Text Name = "";

    IDList Modules;
    ByteArray Values;

    BaseClass(const VTable *Table, IDClass NewID = RandomID, FlagClass NewFlags = Flags::None)
        : Vptr(Table), ID(NewID), Flags(NewFlags)
    {
        Objects.Register(this, NewID);
    };

    ~BaseClass();
    void Destroy();
    void Setup(int32_t Index = -1) { Vptr->Setup(this, Index); }
    bool Run() { return Vptr->Run(this); }
    void AddModule(BaseClass *Object, int32_t Index = -1) { Vptr->AddModule(this, Object, Index); }
    void RemoveModule(BaseClass *Object) { Vptr->RemoveModule(this, Object); }

    static void DefaultSetup(BaseClass *self, int32_t Index) { /* Empty default */ };
    static bool DefaultRun(BaseClass *self)
    {
        ReportError(Status::InvalidType);
        return true;
    };
    static void DefaultAddModule(BaseClass *self, BaseClass *Object, int32_t Index)
    {
        self->Modules.Add(Object, Index);
    };
    static void DefaultRemoveModule(BaseClass *self, BaseClass *Object)
    {
        self->Modules.Remove(Object);
    };

    template <class C>
    C *As() const { return (C *)this; };
    void Save();

    template <class C>
    C ValueGet(int32_t Index) const;
    template <class C>
    bool ValueSet(C Value, int32_t Index = -1);

    ByteArray OutputValues(int32_t Value = 0) const;
    bool InputValues(ByteArray &Input, int32_t Index, uint8_t Value = 0);
};
