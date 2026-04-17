#if defined BOARD_Valu_v2_0
#define ROWNUMBER 6
#define ROWHEIGHT 10
#define OLEDWIDTH 128
#define OLEDHEIGHT 64

enum class DisplayMode : uint8_t
{
    Screensaver = 0,
    Menu = 1,
    Part = 2,
    Edit = 3
};

class OLEDClass : public BaseClass
{
public:
    u8g2_t Driver;

    // UI State (Kept in RAM for speed)
    DisplayMode Mode = DisplayMode::Screensaver;
    uint32_t MenuIndex = 0;
    uint32_t PartIndex = 0;
    uint32_t Cooldown = 0;
    uint8_t ScrollOffset = 0;

    OLEDClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});

    void Setup(uint16_t Index);
    bool Run();

    // Render logic
    void RenderScreensaver();
    void RenderMenu();

    uint16_t GetEntryByIndex(uint8_t index);

    static bool RunBridge(BaseClass *Base) { return static_cast<OLEDClass *>(Base)->Run(); }

    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = OLEDClass::RunBridge};
};

constexpr VTable OLEDClass::Table;

OLEDClass::OLEDClass(const Reference &ID, FlagClass Flags, RunInfo Info)
    : BaseClass(&OLEDClass::Table, ID, Flags, Info)
{
    Type = ObjectTypes::OLED;
    Name = "OLED";

    HW::OLED_Init(&Driver);
    u8g2_SetFont(&Driver, u8g2_font_6x10_tr);

    uint16_t cursor = 0;

    // Root Node {0}
    Values.Set(nullptr, 0, Types::Undefined, cursor++, 0);

    // Input Wiring {0, 0} to {0, 3} - Depth 1
    // These store References (Paths) to the actual button objects/values
    Reference nullRef;
    Values.Set(&nullRef, sizeof(Reference), Types::Reference, cursor++, 1, Tri::Reset, Tri::Set); // Up
    Values.Set(&nullRef, sizeof(Reference), Types::Reference, cursor++, 1, Tri::Reset, Tri::Set); // Down
    Values.Set(&nullRef, sizeof(Reference), Types::Reference, cursor++, 1, Tri::Reset, Tri::Set); // Enter
    Values.Set(&nullRef, sizeof(Reference), Types::Reference, cursor++, 1, Tri::Reset, Tri::Set); // Back
}

// Helper to flip a string in place
void reverse(char *str, int len)
{
    int i = 0, j = len - 1;
    while (i < j)
    {
        char temp = str[i];
        str[i++] = str[j];
        str[j--] = temp;
    }
}

// Minimal integer to string (works for Byte and Integer)
int raw_ltoa(int32_t n, char *str)
{
    int i = 0;
    bool negative = false;
    if (n == 0)
    {
        str[i++] = '0';
        str[i] = '\0';
        return 1;
    }
    if (n < 0)
    {
        negative = true;
        n = -n;
    }
    while (n != 0)
    {
        str[i++] = (n % 10) + '0';
        n = n / 10;
    }
    if (negative)
        str[i++] = '-';
    str[i] = '\0';
    reverse(str, i);
    return i;
}

uint8_t GetPartCount(Types type)
{
    if (type == Types::Vector3D || type == Types::Coord2D)
        return 3;
    if (type == Types::Vector2D)
        return 2;
    if (type == Types::Colour)
        return 4;
    return 0;
}

void OLEDClass::RenderMenu()
{
    uint16_t currentIdx = 5; // Start of the menu tree
    uint8_t logicalItem = 0; // Tracks the "index" of the current menu entry in the tree
    uint8_t screenRow = 0;   // Tracks the physical row being drawn (0 to ROWNUMBER-1)

    // 1. Panning: Skip items that are scrolled off the top
    while (currentIdx != INVALID_HEADER && logicalItem < ScrollOffset)
    {
        currentIdx = Values.Next(currentIdx);
        logicalItem++;
    }

    // 2. Draw Loop: Render until we hit the end of the tree or fill the screen
    while (currentIdx != INVALID_HEADER && screenRow < ROWNUMBER)
    {
        Result labelRes = Values.Get(currentIdx);
        if (labelRes.Value)
        {
            int16_t y = (screenRow + 1) * ROWHEIGHT;

            // --- 1. Selection Cursor ---
            if (logicalItem == MenuIndex)
            {
                if (Mode == DisplayMode::Edit)
                    u8g2_DrawStr(&Driver, 0, y, "X");
                else if (Mode == DisplayMode::Part)
                    u8g2_DrawStr(&Driver, 0, y, "#");
                else
                    u8g2_DrawStr(&Driver, 0, y, ">");
            }

            // --- 2. Draw Label ---
            char labelBuf[20];
            uint16_t len = (labelRes.Length < 19) ? labelRes.Length : 19;
            memcpy(labelBuf, labelRes.Value, len);
            labelBuf[len] = '\0';
            u8g2_DrawStr(&Driver, 10, y, labelBuf);

            // --- 3. Draw Value (Child) ---
            uint16_t valIdx = Values.Child(currentIdx);
            if (valIdx != INVALID_HEADER)
            {
                Result v = Values.GetThis(valIdx);
                if (v.Value)
                {
                    // Increment row to draw the value on the line below the label
                    screenRow++;
                    
                    // Safety: Stop if the value row would be off-screen
                    if (screenRow >= ROWNUMBER) break;

                    int16_t valY = (screenRow + 1) * ROWHEIGHT;
                    char vBuf[16];

                    // Highlight for non-packed editing
                    if (logicalItem == MenuIndex && Mode == DisplayMode::Edit && !IsPacked(v.Type))
                    {
                        u8g2_DrawFrame(&Driver, 18, valY - (ROWHEIGHT - 3), 50, ROWHEIGHT);
                    }

                    // Value Rendering based on Type
                    switch (v.Type)
                    {
                    case Types::Bool:
                        u8g2_DrawStr(&Driver, 20, valY, *(bool *)v.Value ? "ON" : "OFF");
                        break;

                    case Types::Byte:
                        raw_ltoa(*(uint8_t *)v.Value, vBuf);
                        u8g2_DrawStr(&Driver, 20, valY, vBuf);
                        break;

                    case Types::Integer:
                        raw_ltoa(*(int32_t *)v.Value, vBuf);
                        u8g2_DrawStr(&Driver, 20, valY, vBuf);
                        break;

                    case Types::Colour:
                    {
                        int16_t xOff = 20;
                        for (uint8_t i = 0; i < 4; i++)
                        {
                            raw_ltoa(((uint8_t *)v.Value)[i], vBuf);
                            if (logicalItem == MenuIndex && i == PartIndex && (Mode == DisplayMode::Part || Mode == DisplayMode::Edit))
                                u8g2_DrawFrame(&Driver, xOff - 1, valY - (ROWHEIGHT - 3), 18, ROWHEIGHT);

                            u8g2_DrawStr(&Driver, xOff, valY, vBuf);
                            xOff += 22;
                        }
                        break;
                    }

                    case Types::Vector2D:
                    case Types::Vector3D:
                    case Types::Coord2D:
                    {
                        uint8_t parts = GetPartCount(v.Type);
                        int16_t xOff = 20;
                        for (uint8_t i = 0; i < parts; i++)
                        {
                            Number n = ((Number *)v.Value)[i];
                            raw_ltoa(n.RoundToInt(), vBuf);

                            if (logicalItem == MenuIndex && i == PartIndex && (Mode == DisplayMode::Part || Mode == DisplayMode::Edit))
                                u8g2_DrawFrame(&Driver, xOff - 1, valY - (ROWHEIGHT - 3), 22, ROWHEIGHT);

                            u8g2_DrawStr(&Driver, xOff, valY, vBuf);
                            xOff += 26;
                        }
                        break;
                    }

                    case Types::Number:
                    {
                        Number n = *(Number *)v.Value;
                        raw_ltoa(n.RoundToInt(), vBuf);
                        u8g2_DrawStr(&Driver, 20, valY, vBuf);
                        break;
                    }

                    default:
                        u8g2_DrawStr(&Driver, 20, valY, "...");
                        break;
                    }
                }
            }
        }
        // Move to the next item in the tree
        currentIdx = Values.Next(currentIdx);
        logicalItem++;
        screenRow++;
    }
}

uint16_t OLEDClass::GetEntryByIndex(uint8_t index)
{
    uint16_t curr = 5; // Start of menu
    for (uint8_t i = 0; i < index && curr != INVALID_HEADER; i++)
    {
        curr = Values.Next(curr);
    }
    return curr;
}

bool OLEDClass::Run()
{
    // 1. Resolve Inputs
    bool btnUp = Values.GetThis(1).Value ? *(bool *)Values.GetThis(1).Value : false;
    bool btnDown = Values.GetThis(2).Value ? *(bool *)Values.GetThis(2).Value : false;
    bool btnEnter = Values.GetThis(3).Value ? *(bool *)Values.GetThis(3).Value : false;
    bool btnBack = Values.GetThis(4).Value ? *(bool *)Values.GetThis(4).Value : false;

    if (CurrentTime - Cooldown < 250)
        return true; // Slightly faster for editing

    if (btnUp || btnDown || btnEnter || btnBack)
    {
        Cooldown = CurrentTime;
        uint16_t entryIdx = GetEntryByIndex(MenuIndex);
        uint16_t valIdx = (entryIdx != INVALID_HEADER) ? Values.Child(entryIdx) : INVALID_HEADER;
        Result v = (valIdx != INVALID_HEADER) ? Values.GetThis(valIdx) : Result{nullptr, 0, Types::Undefined};

        if (Mode == DisplayMode::Menu)
        {
            if (btnDown && MenuIndex > 0)
                MenuIndex--;
            if (btnUp && entryIdx != INVALID_HEADER && Values.Next(entryIdx) != INVALID_HEADER)
                MenuIndex++;
            if (btnEnter && v.Value)
                Mode = IsPacked(v.Type) ? DisplayMode::Part : DisplayMode::Edit;
            if (btnBack)
                Mode = DisplayMode::Screensaver;

            const uint8_t visibleRows = 3; // Adjust this based on your ROWNUMBER and value rows
            if (MenuIndex < ScrollOffset)
            {
                ScrollOffset = MenuIndex;
            }
            else if (MenuIndex >= ScrollOffset + visibleRows)
            {
                ScrollOffset = MenuIndex - visibleRows + 1;
            }
        }
        else if (Mode == DisplayMode::Part)
        {
            if (btnDown && PartIndex > 0)
                PartIndex--;
            if (btnUp && PartIndex < 2)
                PartIndex++;
            if (btnEnter)
                Mode = DisplayMode::Edit;
            if (btnBack)
                Mode = DisplayMode::Menu;
        }
        else if (Mode == DisplayMode::Edit)
        {
            if (v.Value)
            {
                // 1. Fixed-Point Packed Types (Vector2, Vector3, Coord2D)
                if (v.Type == Types::Vector2D || v.Type == Types::Vector3D || v.Type == Types::Coord2D)
                {
                    Number *target = &((Number *)v.Value)[PartIndex];
                    if (btnUp)
                        *target += Number(1);
                    if (btnDown)
                        *target -= Number(1);
                }
                // 2. Single Fixed-Point Number
                else if (v.Type == Types::Number)
                {
                    Number *target = (Number *)v.Value;
                    if (btnUp)
                        *target += Number(1);
                    if (btnDown)
                        *target -= Number(1);
                }
                // 3. Byte-based Packed Types (Colour)
                else if (v.Type == Types::Colour)
                {
                    uint8_t *target = &((uint8_t *)v.Value)[PartIndex];
                    if (btnUp)
                        (*target)++;
                    if (btnDown)
                        (*target)--;
                }
                // 4. Standard Integers
                else if (v.Type == Types::Integer)
                {
                    int32_t *target = (int32_t *)v.Value;
                    if (btnUp)
                        (*target)++;
                    if (btnDown)
                        (*target)--;
                }
                else if (v.Type == Types::Byte)
                {
                    uint8_t *target = (uint8_t *)v.Value;
                    if (btnUp)
                        (*target)++;
                    if (btnDown)
                        (*target)--;
                }
                else if (v.Type == Types::Bool && (btnUp || btnDown))
                {
                    *(bool *)v.Value = !(*(bool *)v.Value);
                }
            }

            if (btnEnter || btnBack)
            {
                Mode = IsPacked(v.Type) ? DisplayMode::Part : DisplayMode::Menu;
            }
        }
        else if (Mode == DisplayMode::Screensaver && btnEnter)
            Mode = DisplayMode::Menu;
    }

    // 3. Render
    u8g2_ClearBuffer(&Driver);
    if (Mode == DisplayMode::Screensaver)
    {
        u8g2_DrawStr(&Driver, 10, 30, "SLEEPING");
    }
    else
    {
        // Edit and Part modes now stay in the menu visually
        RenderMenu();
    }
    u8g2_SendBuffer(&Driver);

    return true;
}

#endif