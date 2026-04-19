#if defined BOARD_Valu_v2_0
#define ROWNUMBER 6
#define ROWHEIGHT 10
#define OLEDWIDTH 128
#define OLEDHEIGHT 64
#define VISIBLE_ROWS 3

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
    uint8_t PageIndex = 0;

    OLEDClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});

    void Setup(uint16_t Index);
    bool Run();

    // Render logic
    void RenderScreensaver();
    void RenderMenu();

    Bookmark GetPageRoot(); // Helper to find the current page node
    Bookmark GetEntryByIndex(uint8_t index);

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

int raw_ntoa(Number n, char *str)
{
    int pos = 0;

    // 1. Handle the negative sign separately for the whole number
    if (n < N(0))
    {
        str[pos++] = '-';
        n = N(0) - n; // Work with absolute value from here on
    }

    // 2. Truncate to get the whole number
    int32_t integral = n.ToInt();
    Number fracPart = n - Number(integral);

    // 3. Round the fraction to 2 decimal places
    int32_t fracInt = (fracPart * N(100)).RoundToInt();

    // 4. Handle rounding overflow (e.g., 0.999 -> 1.00)
    if (fracInt >= 100)
    {
        integral += 1;
        fracInt = 0;
    }

    // 5. Convert integral part
    pos += raw_ltoa(integral, str + pos);

    // 6. Append decimal and fraction
    str[pos++] = '.';
    if (fracInt < 10)
        str[pos++] = '0';

    pos += raw_ltoa(fracInt, str + pos);

    str[pos] = '\0'; // Always null-terminate for safety
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
    Bookmark pageRoot = GetPageRoot();
    if (pageRoot.Index == INVALID_HEADER)
        return;

    // 1. Calculate Total Items (1 Virtual Page Switcher + Children)
    uint16_t entryStartIdx = Values.Child(pageRoot.Index);
    uint16_t countIdx = entryStartIdx;
    uint8_t totalItems = 1;
    while (countIdx != INVALID_HEADER)
    {
        countIdx = Values.Next(countIdx);
        totalItems++;
    }

    uint8_t screenRow = 0, logicalItem = 0;
    uint16_t currentIdx = entryStartIdx;

    // 2. Advance pointers to ScrollOffset
    while (logicalItem < ScrollOffset)
    {
        if (logicalItem > 0 && currentIdx != INVALID_HEADER)
            currentIdx = Values.Next(currentIdx);
        logicalItem++;
    }

    // 3. Draw Loop
    while (screenRow < ROWNUMBER && logicalItem < totalItems)
    {
        int16_t y = (screenRow + 1) * ROWHEIGHT;

        if (logicalItem == 0)
        {
            // --- VIRTUAL PAGE SWITCHER (P#:Name) ---
            if (MenuIndex == 0)
            {
                u8g2_DrawStr(&Driver, 0, y, (Mode == DisplayMode::Edit) ? "X" : ">");
            }

            char pBuf[24];
            pBuf[0] = 'P';
            char numBuf[4];
            raw_ltoa(PageIndex + 1, numBuf);

            uint8_t pos = 1;
            for (uint8_t i = 0; numBuf[i] != '\0' && pos < 5; i++)
                pBuf[pos++] = numBuf[i];
            pBuf[pos++] = ':';

            Result pageRes = pageRoot.Get();
            if (pageRes.Value)
            {
                uint16_t nameLen = (pageRes.Length < (23 - pos)) ? pageRes.Length : (23 - pos);
                memcpy(&pBuf[pos], pageRes.Value, nameLen);
                pBuf[pos + nameLen] = '\0';
            }
            else
            {
                pBuf[pos] = '\0';
            }

            u8g2_DrawStr(&Driver, 10, y, pBuf);
            u8g2_DrawHLine(&Driver, 10, y + 2, 100);
        }
        else
        {
            // --- REAL ENTRIES ---
            if (currentIdx != INVALID_HEADER)
            {
                Bookmark entry = Values.This(currentIdx);
                Result labelRes = entry.Get();

                // Draw Selection Indicator
                if (logicalItem == MenuIndex)
                {
                    const char *cur = (Mode == DisplayMode::Edit) ? "X" : (Mode == DisplayMode::Part ? "#" : ">");
                    u8g2_DrawStr(&Driver, 0, y, cur);
                }

                // Draw Entry Label
                char labelBuf[20];
                uint16_t len = (labelRes.Length < 19) ? labelRes.Length : 19;
                memcpy(labelBuf, labelRes.Value, len);
                labelBuf[len] = '\0';
                u8g2_DrawStr(&Driver, 10, y, labelBuf);

                // Check for Value (Child of current entry)
                Bookmark valBookmark = entry.Child();
                if (valBookmark.Index != INVALID_HEADER)
                {
                    Result v = valBookmark.GetThis();
                    screenRow++;
                    if (screenRow >= ROWNUMBER) break;

                    int16_t valY = (screenRow + 1) * ROWHEIGHT;
                    char vBuf[16];

                    // Draw Step/Edit Indicator
                    if (logicalItem == MenuIndex && Mode == DisplayMode::Edit)
                    {
                        const char *sym = (Step == N(0.01)) ? ":" : (Step == N(0.1) ? "." : (Step == N(1.0) ? "-" : (Step == N(10.0) ? "o" : "O")));
                        u8g2_DrawStr(&Driver, 0, valY, sym);
                    }

                    // Render Value based on Type
                    if (v.Type == Types::Bool)
                    {
                        if (logicalItem == MenuIndex && Mode == DisplayMode::Edit)
                            u8g2_DrawHLine(&Driver, 20, valY + 1, 18);
                        u8g2_DrawStr(&Driver, 20, valY, *(bool *)v.Value ? "ON" : "OFF");
                    }
                    else if (v.Type == Types::Byte || v.Type == Types::Integer)
                    {
                        raw_ltoa((v.Type == Types::Byte) ? *(uint8_t *)v.Value : *(int32_t *)v.Value, vBuf);
                        if (logicalItem == MenuIndex && Mode == DisplayMode::Edit)
                            u8g2_DrawHLine(&Driver, 20, valY + 1, 25);
                        u8g2_DrawStr(&Driver, 20, valY, vBuf);
                    }
                    else if (v.Type == Types::Number || IsPacked(v.Type))
                    {
                        uint8_t parts = (v.Type == Types::Number) ? 1 : GetPartCount(v.Type);
                        int16_t xOff = 20;
                        for (uint8_t i = 0; i < parts; i++)
                        {
                            Number n = (v.Type == Types::Number) ? *(Number *)v.Value : ((Number *)v.Value)[i];
                            raw_ntoa(n, vBuf);
                            
                            // Highlight specific part if in Part/Edit mode
                            if (logicalItem == MenuIndex && i == PartIndex && (Mode == DisplayMode::Part || Mode == DisplayMode::Edit))
                                u8g2_DrawHLine(&Driver, xOff, valY + 1, 30);
                            
                            u8g2_DrawStr(&Driver, xOff, valY, vBuf);
                            xOff += 36;
                        }
                    }
                }
                currentIdx = Values.Next(currentIdx);
            }
        }
        logicalItem++;
        screenRow++;
    }

    // --- SCROLLBAR ---
    int handleH = 64 / totalItems;
    if (handleH < 6) handleH = 6;
    int handleY = (ScrollOffset * (64 - handleH)) / (totalItems > 1 ? (totalItems - 1) : 1);
    u8g2_DrawVLine(&Driver, 127, handleY, handleH);
}

Bookmark OLEDClass::GetPageRoot()
{
    uint16_t curr = 5; // Absolute root of all pages
    for (uint8_t i = 0; i < PageIndex && curr != INVALID_HEADER; i++)
    {
        curr = Values.Next(curr);
    }
    return Values.This(curr); // Returns a Bookmark (Map + Index)
}

Bookmark OLEDClass::GetEntryByIndex(uint8_t index)
{
    Bookmark root = GetPageRoot();
    if (root.Index == INVALID_HEADER)
        return root;

    uint16_t curr = Values.Child(root.Index);
    for (uint8_t i = 0; i < index && curr != INVALID_HEADER; i++)
    {
        curr = Values.Next(curr);
    }
    return Values.This(curr);
}

bool OLEDClass::Run()
{
    bool btnUp = Values.GetThis(1).Value ? *(bool *)Values.GetThis(1).Value : false;
    bool btnDown = Values.GetThis(2).Value ? *(bool *)Values.GetThis(2).Value : false;
    bool btnEnter = Values.GetThis(3).Value ? *(bool *)Values.GetThis(3).Value : false;
    bool btnBack = Values.GetThis(4).Value ? *(bool *)Values.GetThis(4).Value : false;

    if (CurrentTime - Cooldown < 250)
        return true;

    if (btnUp || btnDown || btnEnter || btnBack) {
        Cooldown = CurrentTime;

        // Fetch current context using Bookmarks
        Bookmark entry = (MenuIndex > 0) ? GetEntryByIndex(MenuIndex - 1) : Bookmark{nullptr, INVALID_HEADER};
        Bookmark valBookmark = entry.Child();
        Result v = valBookmark.GetThis();

        if (Mode == DisplayMode::Menu) {
            if (btnDown && MenuIndex > 0) {
                MenuIndex--;
            }
            if (btnUp) {
                if (MenuIndex == 0) {
                    // Peek if there is at least one child entry to move into
                    if (Values.Child(GetPageRoot().Index) != INVALID_HEADER) MenuIndex++;
                } else if (entry.Next().Index != INVALID_HEADER) {
                    MenuIndex++;
                }
            }
            if (btnEnter) {
                if (MenuIndex == 0) Mode = DisplayMode::Edit;
                else if (v.Value) Mode = IsPacked(v.Type) ? DisplayMode::Part : DisplayMode::Edit;
            }
            if (btnBack) Mode = DisplayMode::Screensaver;

            // Update Scrolling
            if (MenuIndex < ScrollOffset) ScrollOffset = MenuIndex;
            else if (MenuIndex >= ScrollOffset + VISIBLE_ROWS) ScrollOffset = MenuIndex - VISIBLE_ROWS + 1;
        }
        else if (Mode == DisplayMode::Part) {
            uint8_t maxParts = GetPartCount(v.Type);
            if (btnDown && PartIndex > 0) PartIndex--;
            if (btnUp && PartIndex < (maxParts - 1)) PartIndex++;
            if (btnEnter) Mode = DisplayMode::Edit;
            if (btnBack) Mode = DisplayMode::Menu;
        }
        else if (Mode == DisplayMode::Edit) {
            if (MenuIndex == 0) {
                // --- PAGE SWITCHING ---
                if (btnUp) {
                    uint8_t oldPage = PageIndex;
                    PageIndex++;
                    if (GetPageRoot().Index == INVALID_HEADER) PageIndex = oldPage;
                    else { ScrollOffset = 0; MenuIndex = 0; }
                }
                if (btnDown && PageIndex > 0) {
                    PageIndex--;
                    ScrollOffset = 0;
                    MenuIndex = 0;
                }
            }
            else if (v.Value) {
                // --- VALUE ADJUSTMENT ---
                if (btnEnter) {
                    // Step Cycling Logic
                    if (v.Type == Types::Number || IsPacked(v.Type)) {
                        if (Step == N(0.01)) Step = N(0.1);
                        else if (Step == N(0.1)) Step = N(1.0);
                        else if (Step == N(1.0)) Step = N(10.0);
                        else if (Step == N(10.0)) Step = N(100.0);
                        else Step = N(0.01);
                    } else {
                        if (Step == N(1.0)) Step = N(10.0);
                        else if (Step == N(10.0)) Step = N(100.0);
                        else Step = N(1.0);
                    }
                }

                // Apply Changes
                if (v.Type == Types::Number || IsPacked(v.Type)) {
                    Number *target = (v.Type == Types::Number) ? (Number *)v.Value : &((Number *)v.Value)[PartIndex];
                    if (btnUp) *target += Step;
                    if (btnDown) *target -= Step;
                }
                else if (v.Type == Types::Byte || v.Type == Types::Colour) {
                    uint8_t *target = (v.Type == Types::Colour) ? &((uint8_t *)v.Value)[PartIndex] : (uint8_t *)v.Value;
                    int16_t stepVal = (int16_t)Step.RoundToInt();
                    if (btnUp) *target += stepVal;
                    if (btnDown) *target -= stepVal;
                }
                else if (v.Type == Types::Integer) {
                    int32_t *target = (int32_t *)v.Value;
                    if (btnUp) *target += Step.RoundToInt();
                    if (btnDown) *target -= Step.RoundToInt();
                }
                else if (v.Type == Types::Bool && (btnUp || btnDown)) {
                    *(bool *)v.Value = !(*(bool *)v.Value);
                }
            }

            if (btnBack) {
                Mode = (MenuIndex == 0) ? DisplayMode::Menu : (IsPacked(v.Type) ? DisplayMode::Part : DisplayMode::Menu);
                Step = N(1.0);

                // --- CALLBACK TRIGGER ---
                if (MenuIndex > 0 && valBookmark.Index != INVALID_HEADER) {
                    // The operation/callback is the sibling of the value
                    Bookmark callback = valBookmark.Next();
                    if (callback.Index != INVALID_HEADER) {
                        RunOperation(callback);
                    }
                }
            }
        }
        else if (Mode == DisplayMode::Screensaver && btnEnter) {
            Mode = DisplayMode::Menu;
        }
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