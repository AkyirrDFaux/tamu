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
    Number Step = N(1.0);

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

// Minimal Number to string (0.01 precision)
int raw_ntoa(Number n, char *str)
{
    int32_t integral = n.RoundToInt();
    // Get the fractional part (absolute)
    Number fracPart = n - Number(integral);
    if (fracPart < N(0))
        fracPart = N(0) - fracPart;

    // Scale 0.01 to an integer 1
    int32_t fracInt = (fracPart * N(100)).RoundToInt();

    int pos = raw_ltoa(integral, str);
    str[pos++] = '.';

    if (fracInt < 10)
        str[pos++] = '0';
    pos += raw_ltoa(fracInt, str + pos);
    return pos;
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
    // --- 1. Pre-calculation for Scaling Scrollbar ---
    uint16_t countIdx = 5;
    uint8_t totalItems = 0;
    while (countIdx != INVALID_HEADER)
    {
        countIdx = Values.Next(countIdx);
        totalItems++;
    }
    if (totalItems == 0)
        totalItems = 1;

    uint16_t currentIdx = 5;
    uint8_t logicalItem = 0;
    uint8_t screenRow = 0;

    while (currentIdx != INVALID_HEADER && logicalItem < ScrollOffset)
    {
        currentIdx = Values.Next(currentIdx);
        logicalItem++;
    }

    // --- 2. Draw Loop ---
    while (currentIdx != INVALID_HEADER && screenRow < ROWNUMBER)
    {
        Result labelRes = Values.Get(currentIdx);
        if (labelRes.Value)
        {
            int16_t y = (screenRow + 1) * ROWHEIGHT;

            // Main Cursor Column (X=0)
            if (logicalItem == MenuIndex)
            {
                if (Mode == DisplayMode::Edit)
                    u8g2_DrawStr(&Driver, 0, y, "X");
                else if (Mode == DisplayMode::Part)
                    u8g2_DrawStr(&Driver, 0, y, "#");
                else
                    u8g2_DrawStr(&Driver, 0, y, ">");
            }

            // Draw Label
            char labelBuf[20];
            uint16_t len = (labelRes.Length < 19) ? labelRes.Length : 19;
            memcpy(labelBuf, labelRes.Value, len);
            labelBuf[len] = '\0';
            u8g2_DrawStr(&Driver, 10, y, labelBuf);

            // Draw Value Row
            uint16_t valIdx = Values.Child(currentIdx);
            if (valIdx != INVALID_HEADER)
            {
                Result v = Values.GetThis(valIdx);
                if (v.Value)
                {
                    screenRow++;
                    if (screenRow >= ROWNUMBER)
                        break;
                    int16_t valY = (screenRow + 1) * ROWHEIGHT;
                    char vBuf[16];

                    // --- NEW: Step Symbol under the Cursor ---
                    if (logicalItem == MenuIndex && Mode == DisplayMode::Edit)
                    {
                        const char *sym = "o";
                        if (Step == N(0.01))
                            sym = ":";
                        else if (Step == N(0.1))
                            sym = ".";
                        else if (Step == N(1.0))
                            sym = "-";
                        else if (Step == N(10.0))
                            sym = "o";
                        else if (Step == N(100.0))
                            sym = "O";
                        // Drawn at X=0, same column as the cursor but on the value's line
                        u8g2_DrawStr(&Driver, 0, valY, sym);
                    }

                    switch (v.Type)
                    {
                    case Types::Bool:
                        if (logicalItem == MenuIndex && Mode == DisplayMode::Edit)
                            u8g2_DrawHLine(&Driver, 20, valY + 1, 18);
                        u8g2_DrawStr(&Driver, 20, valY, *(bool *)v.Value ? "ON" : "OFF");
                        break;

                    case Types::Byte:
                    case Types::Integer:
                        raw_ltoa((v.Type == Types::Byte) ? *(uint8_t *)v.Value : *(int32_t *)v.Value, vBuf);
                        if (logicalItem == MenuIndex && Mode == DisplayMode::Edit)
                            u8g2_DrawHLine(&Driver, 20, valY + 1, 25);
                        u8g2_DrawStr(&Driver, 20, valY, vBuf);
                        break;

                    case Types::Colour:
                    {
                        int16_t xOff = 20;
                        for (uint8_t i = 0; i < 4; i++)
                        {
                            raw_ltoa(((uint8_t *)v.Value)[i], vBuf);
                            if (logicalItem == MenuIndex && i == PartIndex && (Mode == DisplayMode::Part || Mode == DisplayMode::Edit))
                                u8g2_DrawHLine(&Driver, xOff, valY + 1, 16);
                            u8g2_DrawStr(&Driver, xOff, valY, vBuf);
                            xOff += 22;
                        }
                        break;
                    }

                    case Types::Vector2D:
                    case Types::Vector3D:
                    case Types::Coord2D:
                    case Types::Number:
                    {
                        uint8_t parts = (v.Type == Types::Number) ? 1 : GetPartCount(v.Type);
                        int16_t xOff = 20;
                        for (uint8_t i = 0; i < parts; i++)
                        {
                            Number n = (v.Type == Types::Number) ? *(Number *)v.Value : ((Number *)v.Value)[i];
                            raw_ntoa(n, vBuf);
                            if (logicalItem == MenuIndex && i == PartIndex && (Mode == DisplayMode::Part || Mode == DisplayMode::Edit))
                                u8g2_DrawHLine(&Driver, xOff, valY + 1, 30);
                            u8g2_DrawStr(&Driver, xOff, valY, vBuf);
                            xOff += 36;
                        }
                        break;
                    }
                    }
                }
            }
        }
        currentIdx = Values.Next(currentIdx);
        logicalItem++;
        screenRow++;
    }

    // --- 3. Dynamic Scrollbar Logic ---
    int handleH = 64 / totalItems;
    if (handleH < 6)
        handleH = 6; // Minimum grip size
    // Calculate Y based on the range (0 to 64-handleH)
    int scrollRange = 64 - handleH;
    int handleY = (ScrollOffset * scrollRange) / (totalItems > 1 ? (totalItems - 1) : 1);

    u8g2_DrawVLine(&Driver, 127, handleY, handleH);
    u8g2_DrawVLine(&Driver, 126, handleY, handleH);
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

static const unsigned char logoV2_48px[] = {
    0x0c, 0x00, 0x00, 0x00, 0x00, 0x30, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x38, 0x3c, 0x00, 0x00, 0x00,
    0x00, 0x3c, 0x78, 0x00, 0x00, 0x00, 0x00, 0x1e, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xf8, 0x01,
    0x00, 0x00, 0x80, 0x1f, 0xf0, 0x03, 0x00, 0x00, 0xc0, 0x0f, 0xb0, 0x07, 0x00, 0x00, 0xe0, 0x0d,
    0x30, 0x0f, 0x00, 0x00, 0xf0, 0x0c, 0x60, 0x1e, 0x00, 0x00, 0x78, 0x06, 0x60, 0x3e, 0x00, 0x00,
    0x7c, 0x06, 0x60, 0x7c, 0x00, 0x00, 0x3e, 0x06, 0xc0, 0xfc, 0x00, 0x00, 0x3f, 0x03, 0xc0, 0xfc,
    0x01, 0x80, 0x3f, 0x03, 0xc0, 0xf9, 0x03, 0xc0, 0x9f, 0x03, 0x80, 0xf9, 0xff, 0xff, 0x9f, 0x01,
    0x80, 0xf1, 0xff, 0xff, 0x8f, 0x01, 0x80, 0xf3, 0xff, 0xff, 0xcf, 0x01, 0x00, 0xf3, 0xff, 0xff,
    0xcf, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0xfe, 0xff, 0xff, 0x7f, 0x00, 0x00, 0xff,
    0xff, 0xff, 0xff, 0x00, 0x80, 0xff, 0xff, 0xff, 0xff, 0x01, 0xc0, 0xff, 0xff, 0xff, 0xff, 0x03,
    0xc0, 0xff, 0xff, 0xff, 0xff, 0x03, 0x80, 0x1f, 0xf8, 0x1f, 0xf8, 0x01, 0x00, 0xff, 0xf1, 0x8f,
    0xff, 0x00, 0x00, 0xfe, 0xe5, 0xa7, 0x7f, 0x00, 0x00, 0xfc, 0xed, 0xb7, 0x3f, 0x00, 0x00, 0xf8,
    0xff, 0xff, 0x1f, 0x00, 0x00, 0xf0, 0xff, 0xff, 0x0f, 0x00, 0x00, 0xe0, 0xff, 0xff, 0x07, 0x00,
    0x00, 0xc0, 0xff, 0xff, 0x03, 0x00, 0x00, 0x80, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0xfe, 0x7f, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x3f, 0x00, 0x00, 0x00, 0x80,
    0x39, 0x9c, 0x01, 0x00, 0x00, 0x80, 0xd1, 0x8b, 0x01, 0x00, 0x00, 0x00, 0xc0, 0x03, 0x00, 0x00,
    0x00, 0x80, 0x9f, 0xf9, 0x01, 0x00, 0x00, 0x80, 0x7f, 0xfe, 0x01, 0x00, 0x00, 0x80, 0xff, 0xff,
    0x01, 0x00, 0x00, 0x80, 0xff, 0xff, 0x01, 0x00, 0x00, 0x80, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x99, 0x99, 0x01, 0x00, 0x00, 0x80, 0x99, 0x99, 0x01, 0x00};

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
                // --- Enter Cycles Step ---
                if (btnEnter)
                {
                    if (v.Type == Types::Number || v.Type == Types::Vector2D ||
                        v.Type == Types::Vector3D || v.Type == Types::Coord2D)
                    {
                        if (Step == N(0.01))
                            Step = N(0.1);
                        else if (Step == N(0.1))
                            Step = N(1.0);
                        else if (Step == N(1.0))
                            Step = N(10.0);
                        else if (Step == N(10.0))
                            Step = N(100.0);
                        else
                            Step = N(0.01);
                    }
                    else
                    {
                        // Integer types (Byte/Int) don't support < 1
                        if (Step == N(1.0))
                            Step = N(10.0);
                        else if (Step == N(10.0))
                            Step = N(100.0);
                        else
                            Step = N(1.0);
                    }
                }

                // --- Apply Logic ---
                if (v.Type == Types::Vector2D || v.Type == Types::Vector3D || v.Type == Types::Coord2D)
                {
                    Number *target = &((Number *)v.Value)[PartIndex];
                    if (btnUp)
                        *target += Step;
                    if (btnDown)
                        *target -= Step;
                }
                else if (v.Type == Types::Number)
                {
                    Number *target = (Number *)v.Value;
                    if (btnUp)
                        *target += Step;
                    if (btnDown)
                        *target -= Step;
                }
                else if (v.Type == Types::Colour || v.Type == Types::Byte)
                {
                    uint8_t *target = (v.Type == Types::Colour) ? &((uint8_t *)v.Value)[PartIndex] : (uint8_t *)v.Value;
                    int16_t stepVal = (int16_t)Step.RoundToInt();
                    if (btnUp)
                        *target += stepVal;
                    if (btnDown)
                        *target -= stepVal;
                }
                else if (v.Type == Types::Integer)
                {
                    int32_t *target = (int32_t *)v.Value;
                    if (btnUp)
                        *target += Step.RoundToInt();
                    if (btnDown)
                        *target -= Step.RoundToInt();
                }
                else if (v.Type == Types::Bool && (btnUp || btnDown))
                {
                    *(bool *)v.Value = !(*(bool *)v.Value);
                }
            }

            if (btnBack)
            {
                Mode = IsPacked(v.Type) ? DisplayMode::Part : DisplayMode::Menu;
                Step = N(1.0);
            }
        }
        else if (Mode == DisplayMode::Screensaver && btnEnter)
            Mode = DisplayMode::Menu;
    }

    // --- Add this right before u8g2_ClearBuffer ---
    if (Mode == DisplayMode::Screensaver)
    {
        u8g2_SetContrast(&Driver, 30); // Dim for screensaver
    }
    else
    {
        u8g2_SetContrast(&Driver, 255); // Max brightness for UI
    }

    // 3. Render
    u8g2_ClearBuffer(&Driver);

    // 3. Render
    u8g2_ClearBuffer(&Driver);
    if (Mode == DisplayMode::Screensaver)
    {
        // 1. Draw the base logo
        u8g2_SetDrawColor(&Driver, 1);
        u8g2_DrawXBMP(&Driver, 40, 8, 48, 48, logoV2_48px);

        // 2. High-Density XOR Glitch (Independent Lines)
        u8g2_SetDrawColor(&Driver, 2); // XOR Mode

        for (int i = 0; i < 6; i++)
        {
            // Each line gets its own seed by offsetting CurrentTime
            // This makes them jump to new positions at different times
            uint32_t individualSeed = (CurrentTime + (i * 100)) / 512;

            // Each line gets its own independent flicker phase
            bool isFlickerOn = ((CurrentTime + (i * 37)) / 40) % 8;

            // Unique coordinates based on the individual seed
            uint8_t x = (uint8_t)((individualSeed * (131 + i * 23)) % 128);
            uint8_t y = (uint8_t)((individualSeed * (73 + i * 19)) % 64);
            uint8_t w = (uint8_t)(5 + ((individualSeed * (47 + i)) % 35));

            // Logic: Independent chance to exist + independent flicker
            if ((individualSeed + i) % 3 != 0)
            {
                if (isFlickerOn)
                {
                    u8g2_DrawHLine(&Driver, x, y, w);

                    // Add "thickness" to variety
                    if (i % 3 == 0)
                    {
                        u8g2_DrawHLine(&Driver, x, (y + 1) % 64, w);
                    }
                }
            }
        }

        u8g2_SetDrawColor(&Driver, 1); // Reset
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