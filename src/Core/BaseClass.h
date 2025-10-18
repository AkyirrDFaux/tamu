enum Flags : uint8_t
{
    None = 0,
    Auto = 0b00000001,         // No name change, cannot remove, no editing, no saving
    Undefined = 0b00000010,
    Undefined2 = 0b00000100,      
    RunLoop = 0b00001000,      // Allow automatic run forever
    RunOnce = 0b00010000,      // Run once manually until finished, will reset the flag automatically
    RunOnStartup = 0b00100000, // Run automatically once after board finished loading
    Favourite = 0b01000000,    // Show when filtered
    Inactive = 0b10000000      // Ignore object
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
    Types Type = Types::Undefined;
    IDClass ID;
    FlagClass Flags;
    String Name = "";

    uint32_t ReferencesCount = 0;
    IDList Modules;
    DataList Values;

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

    ByteArray GetValue(int32_t Value = 0) const;
    bool SetValue(ByteArray &Input, uint8_t Value = 0);
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
    *this = ByteArray(Data.Type) << ByteArray(Data.ID) << ByteArray(Data.Flags) << ByteArray(Data.Name) << ByteArray(Data.Modules) << Data.GetValue();
};

String BaseClass::ContentDebug(int32_t Level)
{
    String Text = "";
    for (int32_t Index = 0; Index < Level; Index++)
        Text += "-..";

    Text += "ID: ";
    Text += ID.ToString();
    Text += ", Type: ";
    Text += String((uint8_t)Type);
    Text += " , Name: ";
    Text += Name + '\n';

    for (int32_t Index = Modules.FirstValid(); Index < Modules.Length; Modules.Iterate(&Index))
        Text += Modules[Index]->ContentDebug(Level + 1);

    return Text;
};