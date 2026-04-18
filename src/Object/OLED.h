#if defined BOARD_Valu_v2_0
#define ROWNUMBER 6
#define ROWHEIGHT 10
#define OLEDWIDTH 128
#define OLEDHEIGHT 64
#define VISIBLE_ROWS 3

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

// Smaller int to string: returns length and writes to buf
int raw_ltoa(int32_t n, char *str)
{
    char buf[12];
    int i = 0, j = 0;
    bool neg = false;

    if (n == 0)
    {
        str[0] = '0';
        str[1] = '\0';
        return 1;
    }
    if (n < 0)
    {
        neg = true;
        n = -n;
    }

    while (n > 0)
    {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }

    if (neg)
        str[j++] = '-';
    while (i > 0)
        str[j++] = buf[--i];

    str[j] = '\0';
    return j;
}

// Minimal Number to string (0.01 precision)
int raw_ntoa(Number n, char *str)
{
    int32_t integral = n.RoundToInt();
    Number fracPart = n - Number(integral);
    if (fracPart < N(0))
        fracPart = N(0) - fracPart;

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
    uint16_t countIdx = 5;
    uint8_t totalItems = 0;
    while (countIdx != INVALID_HEADER)
    {
        countIdx = Values.Next(countIdx);
        totalItems++;
    }

    uint16_t currentIdx = 5;
    uint8_t logicalItem = 0, screenRow = 0;

    while (currentIdx != INVALID_HEADER && logicalItem < ScrollOffset)
    {
        currentIdx = Values.Next(currentIdx);
        logicalItem++;
    }

    while (currentIdx != INVALID_HEADER && screenRow < ROWNUMBER)
    {
        Result labelRes = Values.Get(currentIdx);
        if (labelRes.Value)
        {
            int16_t y = (screenRow + 1) * ROWHEIGHT;
            bool isCurrent = (logicalItem == MenuIndex);

            // Cursor Column
            if (isCurrent)
            {
                const char *cur = (Mode == DisplayMode::Edit) ? "X" : (Mode == DisplayMode::Part ? "#" : ">");
                u8g2_DrawStr(&Driver, 0, y, cur);
            }

            // Label (Manual copy saves Flash vs memcpy)
            char labelBuf[20];
            uint8_t len = 0;
            while (len < 19 && len < labelRes.Length)
            {
                labelBuf[len] = ((char *)labelRes.Value)[len];
                len++;
            }
            labelBuf[len] = '\0';
            u8g2_DrawStr(&Driver, 10, y, labelBuf);

            uint16_t valIdx = Values.Child(currentIdx);
            if (valIdx != INVALID_HEADER)
            {
                Result v = Values.GetThis(valIdx);
                if (v.Value && ++screenRow < ROWNUMBER)
                {
                    int16_t valY = (screenRow + 1) * ROWHEIGHT;
                    char vBuf[16];

                    if (isCurrent && Mode == DisplayMode::Edit)
                    {
                        char sym = (Step < N(0.1)) ? ':' : (Step < N(1.0) ? '.' : (Step > N(10.0) ? 'O' : 'o'));
                        if (Step == N(1.0))
                            sym = '-';
                        vBuf[0] = sym;
                        vBuf[1] = '\0';
                        u8g2_DrawStr(&Driver, 0, valY, vBuf);
                    }

                    // Grouped Logic
                    if (v.Type == Types::Bool)
                    {
                        u8g2_DrawStr(&Driver, 20, valY, *(bool *)v.Value ? "ON" : "OFF");
                    }
                    else if (v.Type == Types::Byte || v.Type == Types::Integer)
                    {
                        raw_ltoa((v.Type == Types::Byte) ? *(uint8_t *)v.Value : *(int32_t *)v.Value, vBuf);
                        u8g2_DrawStr(&Driver, 20, valY, vBuf);
                    }
                    else
                    { // Packed Types (Vectors, Colour, Number)
                        uint8_t parts = (v.Type == Types::Colour) ? 4 : (v.Type == Types::Number ? 1 : GetPartCount(v.Type));
                        int16_t xOff = 20;
                        for (uint8_t i = 0; i < parts; i++)
                        {
                            if (v.Type == Types::Colour)
                                raw_ltoa(((uint8_t *)v.Value)[i], vBuf);
                            else
                                raw_ntoa((v.Type == Types::Number) ? *(Number *)v.Value : ((Number *)v.Value)[i], vBuf);

                            if (isCurrent && i == PartIndex && Mode != DisplayMode::Menu)
                                u8g2_DrawHLine(&Driver, xOff, valY + 1, (v.Type == Types::Colour ? 16 : 30));

                            u8g2_DrawStr(&Driver, xOff, valY, vBuf);
                            xOff += (v.Type == Types::Colour ? 22 : 36);
                        }
                    }
                }
            }
        }
        currentIdx = Values.Next(currentIdx);
        logicalItem++;
        screenRow++;
    }

    // Scrollbar Logic (Simplified)
    int handleH = 64 / (totalItems ?: 1);
    if (handleH < 6)
        handleH = 6;
    u8g2_DrawVLine(&Driver, 126, (ScrollOffset * (64 - handleH)) / (totalItems > 1 ? totalItems - 1 : 1), handleH);
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
    bool btnUp = Values.GetThis(1).Value ? *(bool *)Values.GetThis(1).Value : false;
    bool btnDown = Values.GetThis(2).Value ? *(bool *)Values.GetThis(2).Value : false;
    bool btnEnter = Values.GetThis(3).Value ? *(bool *)Values.GetThis(3).Value : false;
    bool btnBack = Values.GetThis(4).Value ? *(bool *)Values.GetThis(4).Value : false;

    if (CurrentTime - Cooldown < 250)
        return true;

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

            if (MenuIndex < ScrollOffset)
                ScrollOffset = MenuIndex;
            else if (MenuIndex >= ScrollOffset + VISIBLE_ROWS)
                ScrollOffset = MenuIndex - VISIBLE_ROWS + 1;
        }
        else if (Mode == DisplayMode::Part)
        {
            if (btnDown && PartIndex > 0)
                PartIndex--;
            if (btnUp && PartIndex < 2)
                PartIndex++; // Limit for Vector3D
            if (btnEnter)
                Mode = DisplayMode::Edit;
            if (btnBack)
                Mode = DisplayMode::Menu;
        }
        else if (Mode == DisplayMode::Edit && v.Value)
        {
            if (btnEnter)
            {
                // Step Cycling (Unified)
                if (Step == N(0.01))
                    Step = N(0.1);
                else if (Step == N(0.1))
                    Step = N(1.0);
                else if (Step == N(1.0))
                    Step = N(10.0);
                else if (Step == N(10.0))
                    Step = N(100.0);
                else
                    Step = (v.Type == Types::Number || IsPacked(v.Type)) ? N(0.01) : N(1.0);
            }

            // Apply Logic (Merged)
            if (v.Type == Types::Bool)
            {
                if (btnUp || btnDown)
                    *(bool *)v.Value = !(*(bool *)v.Value);
            }
            else if (btnUp || btnDown)
            { // Only enter here if we are actually changing a value
                int32_t sInt = Step.RoundToInt();
                int8_t dir = btnUp ? 1 : -1;

                if (v.Type >= Types::Byte && v.Type <= Types::Integer)
                {
                    if (v.Type == Types::Byte)
                    {
                        *(uint8_t *)v.Value += (dir * sInt);
                    }
                    else
                    {
                        *(int32_t *)v.Value += (dir * sInt);
                    }
                }
                else if (v.Type >= Types::Number && v.Type <= Types::Coord2D)
                {
                    Number *t = (v.Type == Types::Number) ? (Number *)v.Value : &((Number *)v.Value)[PartIndex];
                    if (btnUp)
                        *t += Step;
                    else
                        *t -= Step;
                }
                else if (v.Type == Types::Colour)
                {
                    ((uint8_t *)v.Value)[PartIndex] += (dir * sInt);
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

    u8g2_SetContrast(&Driver, (Mode == DisplayMode::Screensaver) ? 30 : 255);
    u8g2_ClearBuffer(&Driver);

    if (Mode == DisplayMode::Screensaver)
    {
        u8g2_SetDrawColor(&Driver, 1);
        u8g2_DrawXBMP(&Driver, 40, 8, 48, 48, logoV2_48px);
        u8g2_SetDrawColor(&Driver, 2);

        for (int i = 0; i < 6; i++)
        {
            uint32_t seed = (CurrentTime + (i << 6)) >> 9; // Approx i*64 for phase shift
            if ((seed + i) % 3 != 0 && ((CurrentTime + (i * 37)) / 40) % 8)
            {
                uint8_t x = (seed * (131 + i * 23)) % 128;
                uint8_t y = (seed * (73 + i * 19)) % 64;
                uint8_t w = 5 + ((seed * (47 + i)) % 35);
                u8g2_DrawHLine(&Driver, x, y, w);
                if (i % 3 == 0)
                    u8g2_DrawHLine(&Driver, x, (y + 1) % 64, w);
            }
        }
        u8g2_SetDrawColor(&Driver, 1);
    }
    else
    {
        RenderMenu();
    }

    u8g2_SendBuffer(&Driver);
    return true;
}

#endif