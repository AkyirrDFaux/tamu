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
    void ContentDebug() const;
};

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

    String AsString();
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

class BaseClass
{
public:
    ObjectTypes Type = ObjectTypes::Undefined;
    IDClass ID;
    FlagClass Flags;
    String Name = "";

    IDList Modules;
    ByteArray Values;

    BaseClass(IDClass NewID = RandomID, FlagClass NewFlags = Flags::None);

    virtual ~BaseClass();
    virtual void Setup(int32_t Index = -1) {};
    virtual bool Run()
    {
        ReportError(Status::InvalidType, "Run entry not implemented, Type:" + String((uint8_t)Type));
        return true;
    };

    template <class C>
    C *As() const { return (C *)this; };

    virtual void AddModule(BaseClass *Object, int32_t Index = -1) { Modules.Add(Object, Index); };
    virtual void RemoveModule(BaseClass *Object) { Modules.Remove(Object); };
    void Save();

    template <class C>
    C ValueGet(int32_t Index) const;
    template <class C>
    bool ValueSet(C Value, int32_t Index = -1);

    ByteArray OutputValues(int32_t Value = 0) const;
    bool InputValues(ByteArray &Input, int32_t Index, uint8_t Value = 0);
    String ContentDebug();
};
