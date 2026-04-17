#if defined BOARD_Valu_v2_0
#define ROWNUMBER 12
#define ROWHEIGHT 10
#define OLEDWIDTH 64
#define OLEDHEIGHT 128

enum class DisplayMode : uint8_t {
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

    OLEDClass(const Reference &ID, FlagClass Flags = Flags::RunLoop, RunInfo Info = {1, 0});

    void Setup(uint16_t Index);
    bool Run();

    // Render logic
    void RenderScreensaver();
    void RenderMenu();
    void RenderPart();
    void RenderEdit();

    static bool RunBridge(BaseClass *Base) { return static_cast<OLEDClass *>(Base)->Run(); }

    static constexpr VTable Table = {
        .Setup = BaseClass::DefaultSetup,
        .Run = OLEDClass::RunBridge
    };
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

void OLEDClass::RenderMenu()
{
    // The first menu entry starts at Index 5 (after Root and 4 Button Refs)
    uint16_t currentIdx = 5;
    uint8_t row = 0;

    while (currentIdx != INVALID_HEADER && row < ROWNUMBER)
    {
        Result res = Values.Get(currentIdx);
        
        if (res.Value) 
        {
            // Calculate Y position based on row height
            int16_t yPos = (row + 1) * ROWHEIGHT;

            // Draw selection cursor
            if (row == MenuIndex) {
                u8g2_DrawStr(&Driver, 0, yPos, ">");
            }

            // Determine what string to draw
            // Priority 1: If it's actual Text data
            if (res.Type == Types::Text) {
                // Assuming res.Value points to the char array and res.Length is length
                char buf[20]; // Temporary buffer for safety
                uint16_t len = (res.Length < 19) ? res.Length : 19;
                memcpy(buf, res.Value, len);
                buf[len] = '\0';
                u8g2_DrawStr(&Driver, 10, yPos, buf);
            }
            // Priority 2: Use the Name of the object if we can resolve it
            else {
                // If your system allows resolving a Reference to a Name:
                u8g2_DrawStr(&Driver, 10, yPos, "Object..."); 
            }
        }

        currentIdx = Values.Next(currentIdx);
        row++;
    }
}

bool OLEDClass::Run()
{
    // 1. Resolve Inputs
    bool btnUp    = Values.GetThis(1).Value ? *(bool*)Values.GetThis(1).Value : false;
    bool btnDown  = Values.GetThis(2).Value ? *(bool*)Values.GetThis(2).Value : false;
    bool btnEnter = Values.GetThis(3).Value ? *(bool*)Values.GetThis(3).Value : false;
    bool btnBack  = Values.GetThis(4).Value ? *(bool*)Values.GetThis(4).Value : false;

    // 2. Cooldown
    if (CurrentTime - Cooldown < 200) return true;

    if (btnUp || btnDown || btnEnter || btnBack) {
        Cooldown = CurrentTime;
        
        // Basic Navigation
        if (Mode == DisplayMode::Menu) {
            if (btnUp && MenuIndex > 0) MenuIndex--;
            if (btnDown) MenuIndex++; // You should ideally bound this by the count of siblings
            
            if (btnEnter) Mode = DisplayMode::Edit;
            if (btnBack)  Mode = DisplayMode::Screensaver; 
        }
        else if (Mode == DisplayMode::Edit) {
            if (btnBack) Mode = DisplayMode::Menu;
        }
        else if (Mode == DisplayMode::Screensaver) {
            if (btnEnter) Mode = DisplayMode::Menu;
        }
    }

    // 3. Render
    u8g2_ClearBuffer(&Driver);
    
    switch (Mode) {
        case DisplayMode::Screensaver: 
            u8g2_DrawStr(&Driver, 10, 30, "SLEEPING"); 
            break;
        case DisplayMode::Menu:        
            RenderMenu();        
            break;
        case DisplayMode::Edit:        
            u8g2_DrawStr(&Driver, 10, 10, "EDIT MODE");
            // Logic to show the specific value of MenuIndex
            break;
        default: break;
    }
    
    u8g2_SendBuffer(&Driver);
    return true;
}

#endif