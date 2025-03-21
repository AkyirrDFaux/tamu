enum Flags : uint8_t
{
    None = 0,
    Auto = 0b00000001,    // No name change, cannot remove, no editing, no saving
    RunLoop = 0b00001000, //Allow automatic run forever
    RunOnce = 0b00010000, //Run once manually until finished, will reset the flag automatically
    RunOnStartup = 0b00100000, //Run automatically once after board finished loading
    Favourite = 0b01000000, // Show when filtered
    Inactive = 0b10000000 // Ignore object
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

template <>
ByteArray::ByteArray(const FlagClass &Data)
{
    *this = ByteArray(Data.Values);
};

template <>
ByteArray::operator FlagClass() const
{
    FlagClass New;
    New.Values = SubArray(0);
    return New;
};

class BaseClass
{
public:
    Types Type = Types::Undefined;
    IDClass ID;
    FlagClass Flags;
    String Name = "";

    uint32_t ReferencesCount = 0;
    IDList Modules;

    BaseClass(IDClass NewID = RandomID, FlagClass NewFlags = Flags::None);

    virtual ~BaseClass();
    virtual void Setup() {};
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
    C *ValueAs() const;

    ByteArray GetValue() const;
    bool SetValue(ByteArray *Input);
    String ContentDebug(int32_t Level);
};

BaseClass *CreateObject(Types Type, bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);

void BaseClass::AddModule(BaseClass *Object, int32_t Index)
{
    Modules.Add(Object, Index);
    Object->ReferencesCount++;
};

template <>
ByteArray::ByteArray(const BaseClass &Data)
{
    // Type, ID, Flags, Name, Modules,(+ values)
    *this = ByteArray(Types::Type) << ByteArray(Data.Type);
    *this = *this << ByteArray(Types::ID) << ByteArray(Data.ID);
    *this = *this << ByteArray(Types::Flags) << ByteArray(Data.Flags);
    *this = *this << ByteArray(Types::Text) << ByteArray(Data.Name);
    *this = *this << ByteArray(Types::IDList) << ByteArray(Data.Modules);
    *this = *this << Data.GetValue();
};

String BaseClass::ContentDebug(int32_t Level)
{
    String Text = "";
    for (int32_t Index = 0; Index < Level; Index++)
        Text += "-..";

    Text += "ID: ";
    Text += String(ID.ID);
    Text += ", Type: ";
    Text += String((uint8_t)Type);
    Text += " , Name: ";
    Text += Name + '\n';

    for (int32_t Index = Modules.FirstValid(); Index < Modules.Length; Modules.Iterate(&Index))
        Text += Modules[Index]->ContentDebug(Level + 1);

    return Text;
};

class Folder : public BaseClass
{
public:
    Folder(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None) : BaseClass(ID, Flags)
    {
        Type = Types::Folder;
    };
};