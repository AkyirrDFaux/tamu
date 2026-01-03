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

    void *ValueAt(IDClass ID) const;
    template <class C>
    bool ValueSet(C Value, IDClass ID);
    bool IsValidValue(IDClass ID, Types Filter = Types::Undefined) const;

    void Expand(uint32_t NewAllocated);
    void Shorten();

    bool Register(BaseClass *AddObject, IDClass ID = RandomID);
    bool Unregister(IDClass ID);
    bool Unregister(BaseClass *RemovedObject);
    String ContentDebug() const;
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
    void Iterate(int32_t *Index, ObjectTypes Filter = ObjectTypes::Undefined) const;

    bool IsValidValue(int32_t Index, Types Filter = Types::Undefined) const;
    Types ValueTypeAt(int32_t Index) const;
    void *ValueAt(int32_t Index) const;
    template <class C>
    bool ValueSet(C Value, int32_t Index);

    void Expand(int8_t NewLength);
    void Shorten();

    bool Add(IDClass ID, int32_t Index = -1);
    bool Add(BaseClass *AddObject, int32_t Index);
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

class DataList
{
public:
    Types *Type = nullptr;
    void **Data = nullptr;
    int32_t Length = 0;

    ~DataList() { DeleteAll(); };

    void *operator[](int32_t Index) const;
    template <class C>
    C *At(int32_t Index) const;
    Types TypeAt(int32_t Index) const;
    bool IsValid(int32_t Index, Types TypeCheck = Types::Undefined) const;

    void Expand(int32_t NewLength);
    void Shorten();

    template <class C>
    bool Add(C AddObject, int32_t Index = -1);
    bool Delete(int32_t Index);
    void DeleteAll();
};

class BaseClass
{
public:
    ObjectTypes Type = ObjectTypes::Undefined;
    IDClass ID;
    FlagClass Flags;
    String Name = "";

    uint32_t ReferencesCount = 0;
    IDList Modules;
    DataList Values;

    BaseClass(IDClass NewID = RandomID, FlagClass NewFlags = Flags::None);

    virtual ~BaseClass();
    virtual void Setup(int32_t Index = -1) {};
    virtual bool Run()
    {
        ReportError(Status::InvalidType, "Run entry not implemented, Type:" + String((uint8_t)Type));
        return true;
    };

    void AddModule(BaseClass *Object, int32_t Index = -1);
    void Save();

    template <class C>
    C *As() const { return (C *)this; };

    template <class C>
    bool ValueSet(C Value, int32_t Index);

    ByteArray OutputValues(int32_t Value = 0) const;
    bool InputValues(ByteArray &Input, uint8_t Value = 0);
    String ContentDebug(int32_t Level);
};