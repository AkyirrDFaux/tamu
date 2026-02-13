#define ROWNUMBER 12
#define ROWHEIGHT 10
#define OLEDWIDTH 64
#define OLEDHEIGHT 128
class OLEDClass : public BaseClass
{
public:
    enum Module
    {
        Up,
        Enter,
        Down,
        Back
    };
    enum View
    {
        Screensaver,
        MainMenu,
        Object,
        Value,
        Part,
        Edit
    };
    u8g2_t Driver;
    uint32_t Cooldown = 0;
    View View = Screensaver;
    uint32_t MenuIndex = 0;   // Selects object
    uint32_t ObjectIndex = 0; // Selects part in object (id, name, flag, module, value ...)
    uint32_t ValueIndex = 0;  // Selects value to edit
    uint32_t PartIndex = 0;   // Selects value element to edit (RGB...)
    OLEDClass(IDClass ID = RandomID, FlagClass Flags = Flags::None);
    ~OLEDClass();
    void Setup(int32_t Index = -1);
    bool Run();

    void RenderScreensaver();
    void RenderMainMenu();
    void RenderObject();
    void RenderValue();
    void RenderEdit();

    void IncrementIndex(uint32_t &Index, bool Up, bool Down);
    Types TypeSelected();

    static void SetupBridge(BaseClass *Base, int32_t Index) { static_cast<OLEDClass *>(Base)->Setup(Index); }
    static bool RunBridge(BaseClass *Base) { return static_cast<OLEDClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = OLEDClass::RunBridge,
        .AddModule = BaseClass::DefaultAddModule,
        .RemoveModule = BaseClass::DefaultRemoveModule};
};

constexpr VTable OLEDClass::Table;

OLEDClass::OLEDClass(IDClass ID, FlagClass Flags) : BaseClass(&Table, ID, Flags)
{
    BaseClass::Type = ObjectTypes::OLED;
    Name = "Input";
    HW::OLED_Init(&Driver);
    u8g2_SetFont(&Driver, u8g2_font_6x10_tr);
    Outputs.Add(this);
};

OLEDClass::~OLEDClass()
{
    Outputs.Remove(this);
};

void ltoa(uint32_t n, char *s)
{
    char temp[10];
    int i = 0;

    // Handle 0 explicitly
    if (n == 0)
    {
        *s++ = '0';
        *s = '\0';
        return;
    }

    // Extract digits in reverse order
    while (n > 0)
    {
        temp[i++] = (n % 10) + '0';
        n /= 10;
    }

    // Reverse them into the output buffer
    while (i > 0)
    {
        *s++ = temp[--i];
    }
    *s = '\0'; // Null terminator
}

void ttoa(Text t, char *s)
{
    memcpy(s, t.Data, t.Length);
    s[t.Length] = '\0';
}

void OLEDClass::RenderScreensaver()
{
    u8g2_DrawTriangle(&Driver, 10, 10, 30, 30, 50, 10);
}

void OLEDClass::RenderMainMenu()
{
    if (MenuIndex > Objects.Allocated - 2)
        MenuIndex = Objects.Allocated - 2;

    uint32_t ViewIndex = 0;
    if (MenuIndex < ROWNUMBER / 2)
        ViewIndex = 0;
    else if (MenuIndex > Objects.Allocated - ROWNUMBER / 2)
        ViewIndex = Objects.Allocated - ROWNUMBER;
    else
        ViewIndex = MenuIndex - ROWNUMBER / 2;

    char Temp[20];

    for (uint32_t Index = 0; Index < ROWNUMBER; Index++)
    {

        ltoa(Index + ViewIndex + 1, Temp);
        u8g2_DrawStr(&Driver, 0, (Index + 1) * ROWHEIGHT, Temp);
        IDClass ID = IDClass(Index + ViewIndex + 1, 0);

        if (Objects.IsValid(ID) == false)
            u8g2_DrawStr(&Driver, 20, (Index + 1) * ROWHEIGHT, "-");
        else
        {
            ttoa(Objects[ID]->Name, Temp);
            u8g2_DrawStr(&Driver, 20, (Index + 1) * ROWHEIGHT, Temp);
        }
    }
    u8g2_DrawFrame(&Driver, 0, (MenuIndex - ViewIndex) * ROWHEIGHT + 1, OLEDWIDTH - 1, ROWHEIGHT);
}

void OLEDClass::RenderObject()
{
    if (ObjectIndex > 5)
        ObjectIndex = 5;

    char Temp[20];

    u8g2_DrawStr(&Driver, 0, ROWHEIGHT, "ID");
    ltoa(MenuIndex + 1, Temp);
    u8g2_DrawStr(&Driver, 30, ROWHEIGHT, Temp);

    u8g2_DrawStr(&Driver, 0, 2 * ROWHEIGHT, "Type");
    ltoa((uint8_t)Objects[IDClass(MenuIndex + 1, 0)]->Type, Temp);
    u8g2_DrawStr(&Driver, 30, 2 * ROWHEIGHT, Temp);

    u8g2_DrawStr(&Driver, 0, 3 * ROWHEIGHT, "Name");
    u8g2_DrawStr(&Driver, 0, 4 * ROWHEIGHT, "Flags");
    u8g2_DrawStr(&Driver, 0, 5 * ROWHEIGHT, "Modules");
    u8g2_DrawStr(&Driver, 0, 6 * ROWHEIGHT, "Values");

    u8g2_DrawFrame(&Driver, 0, ObjectIndex * ROWHEIGHT + 1, OLEDWIDTH - 1, ROWHEIGHT);
}

void OLEDClass::RenderValue()
{
    uint32_t Length = 0;
    BaseClass *Object = Objects[IDClass(MenuIndex + 1, 0)];
    if (ObjectIndex == 4)
        Length = Object->Modules.Length;
    else if (ObjectIndex == 5)
        Length = Object->Values.GetNumberOfValues();

    if (Length == 0)
        ValueIndex = 0;
    else if (ValueIndex > Length - 1)
        ValueIndex = Length - 1;

    uint32_t ViewIndex = 0;
    if (ValueIndex < ROWNUMBER / 2)
        ViewIndex = 0;
    else if (ValueIndex > Length - ROWNUMBER / 2)
        ViewIndex = Length - ROWNUMBER;
    else
        ViewIndex = ValueIndex - ROWNUMBER / 2;

    
    char Temp[20];

    for (uint32_t Index = 0; Index < (Length < ROWNUMBER ? Length : ROWNUMBER); Index++)
    {
        ltoa(Index + ViewIndex, Temp);
        u8g2_DrawStr(&Driver, 0, (Index + 1) * ROWHEIGHT, Temp);

        if (ObjectIndex == 4)
        {
            if (Object->Modules.IsValid(Index + ViewIndex) == false)
                u8g2_DrawStr(&Driver, 20, (Index + 1) * ROWHEIGHT, "-");
            else
            {
                IDClass ID = Object->Modules.IDs[Index + ViewIndex];
                ltoa(ID.Base(), Temp);
                u8g2_DrawStr(&Driver, 20, (Index + 1) * ROWHEIGHT, Temp);
                ltoa(ID.Sub(), Temp);
                u8g2_DrawStr(&Driver, 40, (Index + 1) * ROWHEIGHT, Temp);
            }
        }
        else if (ObjectIndex == 5)
        {
            ltoa((uint8_t)Object->Values.Type(Index + ViewIndex), Temp);
            u8g2_DrawStr(&Driver, 20, (Index + 1) * ROWHEIGHT, Temp);
        }
    }
    u8g2_DrawFrame(&Driver, 0, (ValueIndex - ViewIndex) * ROWHEIGHT + 1, OLEDWIDTH - 1, ROWHEIGHT);
}

void OLEDClass::RenderEdit()
{
    BaseClass *Object = Objects[IDClass(MenuIndex + 1, 0)];
    char Temp[20];

    switch (TypeSelected())
    {
    case Types::Bool:
        ltoa((uint8_t)Object->Values.Get<bool>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Byte:
        ltoa((uint8_t)Object->Values.Get<uint8_t>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Integer:
        ltoa((uint32_t)Object->Values.Get<int32_t>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Number:
        ltoa((uint32_t)Object->Values.Get<Number>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Time:
        ltoa((uint32_t)Object->Values.Get<uint32_t>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::ID:
        break;
    case Types::Colour:
        break;
    case Types::Vector2D:
        break;
    case Types::Vector3D:
        break;
    case Types::Coord2D:
        break;
    case Types::Coord3D:
        break;
    case Types::Text:
        break;
    case Types::IDList:
        break;
    case Types::ObjectType:
        ltoa((uint32_t)Object->Values.Get<ObjectTypes>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Function:
        ltoa((uint32_t)Object->Values.Get<Functions>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Flags:
        break;
    case Types::Status:
        ltoa((uint32_t)Object->Values.Get<Status>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Board:
        ltoa((uint32_t)Object->Values.Get<Boards>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Sensor:
        ltoa((uint32_t)Object->Values.Get<SensorTypes>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::PortDriver:
        ltoa((uint32_t)Object->Values.Get<Drivers>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::AccGyr:
        ltoa((uint32_t)Object->Values.Get<GyrAccs>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Input:
        ltoa((uint32_t)Object->Values.Get<Inputs>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::LEDStrip:
        ltoa((uint32_t)Object->Values.Get<LEDStrips>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Texture1D:
        ltoa((uint32_t)Object->Values.Get<Textures1D>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Display:
        ltoa((uint32_t)Object->Values.Get<Displays>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Geometry2D:
        ltoa((uint32_t)Object->Values.Get<Geometries>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::GeometryOperation:
        ltoa((uint32_t)Object->Values.Get<GeometryOperation>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Texture2D:
        ltoa((uint32_t)Object->Values.Get<Textures2D>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Operation:
        ltoa((uint32_t)Object->Values.Get<Operations>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::Program:
        ltoa((uint32_t)Object->Values.Get<ProgramTypes>(ValueIndex), Temp);
        if (PartIndex > 0)
            PartIndex = 0;
        break;
    case Types::PortType:
        break;
    default:
        break;
    }
    u8g2_DrawStr(&Driver, 0, ROWHEIGHT, Temp);
    if (View == View::Edit)
        u8g2_DrawLine(&Driver, 0, (PartIndex + 1) * ROWHEIGHT + 1, OLEDWIDTH, (PartIndex + 1) * ROWHEIGHT + 1);
    else
        u8g2_DrawFrame(&Driver, 0, PartIndex * ROWHEIGHT + 1, OLEDWIDTH - 1, ROWHEIGHT);
    
}

void OLEDClass::IncrementIndex(uint32_t &Index, bool Up, bool Down)
{
    if (Up && Index > 0)
        Index--;
    else if (Down)
        Index++;
}
Types OLEDClass::TypeSelected()
{
    if (ObjectIndex == 2)
        return Types::Text;
    else if (ObjectIndex == 3)
        return Types::Flags;
    else if (ObjectIndex == 4)
        return Types::ID;
    else
        return Objects[IDClass(MenuIndex + 1, 0)]->Values.Type(ValueIndex);
}

bool OLEDClass::Run()
{
    /*if (Modules.TypeAt(Module::Up) != Types::Bool || Modules.ValueTypeAt(Module::Down) != Types::Bool || Modules.ValueTypeAt(Module::Enter) != Types::Bool || Modules.ValueTypeAt(Module::Back) != Types::Bool)
    {
        ReportError(Status::MissingModule, ID);
        return true;
    }*/

    bool Up = Modules[Module::Up]->ValueGet<bool>(InputClass::Input);
    bool Down = Modules[Module::Down]->ValueGet<bool>(InputClass::Input);
    bool Enter = Modules[Module::Enter]->ValueGet<bool>(InputClass::Input);
    bool Back = Modules[Module::Back]->ValueGet<bool>(InputClass::Input);

    if (CurrentTime - Cooldown < 200)
        return true;
    if (Up || Down || Enter || Back)
        Cooldown = CurrentTime;

    switch (View)
    {
    case View::Screensaver:
        if (Enter)
            View = View::MainMenu;
        break;
    case View::MainMenu:
        IncrementIndex(MenuIndex, Up, Down);
        if (Enter)
        {
            View = View::Object;
            ObjectIndex = 0;
        }
        if (Back)
            View = View::Screensaver;
        break;
    case View::Object:
        IncrementIndex(ObjectIndex, Up, Down);
        if (Enter && (ObjectIndex == 0 || ObjectIndex == 1))
            ;
        else if (Enter && (ObjectIndex == 4 || ObjectIndex == 5))
        {
            View = View::Value;
            ValueIndex = 0;
        }
        else if (Enter)
            View = View::Part;
        if (Back)
            View = View::MainMenu;
        break;
    case View::Value:
        IncrementIndex(ValueIndex, Up, Down);
        if (Enter)
        {
            PartIndex = 0;
                View = View::Part;
        }
        if (Back)
            View = View::Object;
        break;
    case View::Part:
        IncrementIndex(PartIndex, Up, Down);
        if (Enter)
            View = View::Edit;
        if (Back && (ObjectIndex == 4 || ObjectIndex == 5))
            View = View::Value;
        else if (Back)
            View = View::Object;
        break;
    case View::Edit:
        // Change value with Up and down
        if (Back)
            View = View::Part;
        break;
    default:
        break;
    }

    u8g2_ClearBuffer(&Driver);
    switch (View)
    {
    case View::Screensaver:
        RenderScreensaver();
        break;
    case View::MainMenu:
        RenderMainMenu();
        break;
    case View::Object:
        RenderObject();
        break;
    case View::Value:
        RenderValue();
        break;
    case View::Part:
    case View::Edit:
        RenderEdit();
        break;
    }
    u8g2_SendBuffer(&Driver);
    return true;
};