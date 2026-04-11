class PortTypeClass
{
public:
    uint32_t Values = Ports::None;
    PortTypeClass() {};
    PortTypeClass(uint32_t Input) : Values(Input) {};
    void operator=(uint32_t Other) { Values = Other; };
    void operator+=(uint32_t Other) { Values |= Other; };
    void operator-=(uint32_t Other) { Values &= ~Other; };
    bool operator==(Ports::Ports Other) const { return Values & Other; };
    bool operator!=(Ports::Ports Other) const { return !(Values & Other); };
};

struct PortDefinition
{
    uint32_t Type;
    Pin PinID;
    Drivers Driver;
};

struct PortMap
{
    uint16_t Port;   // Index of {0, 0, Port}
    uint16_t Pin;    // Index of {0, 0, Port, 0}
    uint16_t Driver; // Index of {0, 0, Port, 1}
    uint16_t Ref;    // Index of {0, 0, Port, 1, 0}
};

#if defined BOARD_Tamu_v2_0
static const PortDefinition Port_Table[] = {
    {Ports::GPIO | Ports::ADC | Ports::PWM, {0, 0}, Drivers::None},
    {Ports::GPIO | Ports::ADC | Ports::PWM, {1, 0}, Drivers::None},
    {Ports::GPIO | Ports::PWM, {9, 0}, Drivers::None},
    {Ports::TOut | Ports::PWM, {10, 0}, Drivers::None},
    {Ports::TOut | Ports::PWM, {6, 0}, Drivers::None},
    {Ports::GPIO | Ports::PWM, {8, 0}, Drivers::None},
    {Ports::GPIO | Ports::PWM, {7, 0}, Drivers::None},
    {Ports::GPIO | Ports::ADC | Ports::PWM, {3, 0}, Drivers::None},
    {Ports::I2C_SDA, {4, 0}, Drivers::None},
    {Ports::I2C_SCL, {5, 0}, Drivers::None},
    {Ports::GPIO | Ports::Internal, LED_NOTIFICATION_PIN, Drivers::None}};
#elif defined BOARD_Valu_v2_0
static const PortDefinition Port_Table[] = {
    // Port Type                              Pin {Index, Port}        Driver
    {Ports::GPIO | Ports::PWM, {14, 'A'}, Drivers::None},             // L1
    {Ports::GPIO | Ports::PWM, {8, 'B'}, Drivers::None},              // L2
    {Ports::GPIO, {10, 'B'}, Drivers::None},                          // L3
    {Ports::GPIO, {11, 'B'}, Drivers::None},                          // L4
    {Ports::GPIO | Ports::PWM | Ports::ADC, {1, 'B'}, Drivers::None}, // L5
    {Ports::TOut, {8, 'A'}, Drivers::None},                           // F

    {Ports::GPIO | Ports::ADC, {6, 'A'}, Drivers::None}, // S1
    {Ports::GPIO | Ports::ADC, {1, 'A'}, Drivers::None}, // S2
    {Ports::GPIO | Ports::ADC, {0, 'A'}, Drivers::None}, // S3

    {Ports::UART_RX, {10, 'A'}, Drivers::None}, // RX
    {Ports::UART_TX, {9, 'A'}, Drivers::None},  // TX
    {Ports::I2C_SCL, {6, 'B'}, Drivers::None},  // SCL
    {Ports::I2C_SDA, {7, 'B'}, Drivers::None},  // SDA

    {Ports::GPIO | Ports::Internal, {15, 'B'}, Drivers::None},            // B1
    {Ports::GPIO | Ports::Internal, {14, 'B'}, Drivers::None},            // B2
    {Ports::GPIO | Ports::Internal, {13, 'B'}, Drivers::None},            // B3
    {Ports::GPIO | Ports::Internal, LED_NOTIFICATION_PIN, Drivers::None}, // B4/LED

    {Ports::SPI_CLK, {5, 'A'}, Drivers::None},  // CLK
    {Ports::SPI_MOSI, {7, 'A'}, Drivers::None}, // MOSI
    {Ports::SPI_DRST, {3, 'A'}, Drivers::None}, // DRST
    {Ports::SPI_DDC, {0, 'B'}, Drivers::None},  // DDC
    {Ports::SPI_CS, {4, 'A'}, Drivers::None}    // CS
};
#endif

#define PORT_COUNT (sizeof(Port_Table) / sizeof(Port_Table[0]))

class BoardClass : public BaseClass
{
public:
    void *DriverArray[PORT_COUNT] = {nullptr};
    BoardClass(const Reference &ID);

    // Updated to use Reference
    void Setup(uint16_t Index);
    bool Run();
    void UpdateLoopTime();

    PortMap GetPortMap(PortNumber Port);

    bool ConnectLED(BaseClass *Object, PortNumber Port, uint8_t Index = 0);
    void DriverLED(PortNumber Port);
    bool DisconnectLED(BaseClass *Object, PortNumber Port);

    bool ConnectI2C(BaseClass *Object, PortNumber SDA, PortNumber SCL);
    void DriverI2C(PortNumber SDA, PortNumber SCL);
    bool DisconnectI2C(BaseClass *Object, PortNumber SDA, PortNumber SCL);

    bool ConnectPin(BaseClass *Object, PortNumber Port);
    void DriverPin(PortNumber Port);
    bool DisconnectPin(BaseClass *Object, PortNumber Port);

    // Bridge functions updated for Reference
    static void SetupBridge(BaseClass *Base, uint16_t Index)
    {
        static_cast<BoardClass *>(Base)->Setup(Index);
    }
    static bool RunBridge(BaseClass *Base)
    {
        return static_cast<BoardClass *>(Base)->Run();
    }

    static constexpr VTable Table = {
        .Setup = BoardClass::SetupBridge,
        .Run = BoardClass::RunBridge};
};

BoardClass::BoardClass(const Reference &ID) : BaseClass(&Table, ID, Flags::Auto | Flags::RunLoop, {1, 0})
{
    Type = ObjectTypes::Board;
    Name = "Board";

#if defined BOARD_Tamu_v2_0
    Boards model = Boards::Tamu_v2_0;
#elif defined BOARD_Valu_v2_0
    Boards model = Boards::Valu_v2_0;
#endif

    // The cursor tracks the physical Index in the HeaderArray.
    // We populate them in perfect linear order (0, 1, 2, 3...)
    uint16_t cursor = 0;

    // 1. Root Node {0} - Depth 0
    Values.Set(&model, sizeof(Boards), Types::Board, cursor++, 0, Tri::Set, Tri::Reset);

    // 2. Ports Container {0, 0} - Depth 1
    // This is the first child of the Root.
    Values.Set(nullptr, 0, Types::Undefined, cursor++, 1, Tri::Set, Tri::Reset);

    // 3. Port Definitions
    // Each port entry consists of a Type (Depth 2), Pin (Depth 3), and Driver (Depth 3).
    for (uint16_t i = 0; i < PORT_COUNT; i++)
    {
        const PortDefinition &p = Port_Table[i];

        // Port Type Node {0, 0, i}
        Values.Set(&p.Type, sizeof(PortTypeClass), Types::PortType, cursor++, 2, Tri::Set, Tri::Reset);

        // Pin Node {0, 0, i, 0} (Child of Type)
        Values.Set(&p.PinID, sizeof(Pin), Types::Pin, cursor++, 3, Tri::Set, Tri::Reset);

        // Driver Node {0, 0, i, 1} (Sibling of Pin)
        Values.Set(&p.Driver, sizeof(Drivers), Types::PortDriver, cursor++, 3, Tri::Set, Tri::Reset);
    }

    // 4. Telemetry: {0, 1} through {0, 5}
    int32_t zeroI = 0;
    Number zeroN(0.0);

    // Boot Time {0, 1} -> Integer
    Values.Set(&zeroI, 4, Types::Integer, cursor++, 1, Tri::Set, Tri::Reset);

    // Avg Time {0, 2} and Max Time {0, 3} -> Number
    Values.Set(&zeroN, sizeof(Number), Types::Number, cursor++, 1, Tri::Set, Tri::Reset);
    Values.Set(&zeroN, sizeof(Number), Types::Number, cursor++, 1, Tri::Set, Tri::Reset);

    // RAM Total {0, 4} and RAM Free {0, 5} -> Integer
    Values.Set(&zeroI, 4, Types::Integer, cursor++, 1, Tri::Set, Tri::Reset);
    Values.Set(&zeroI, 4, Types::Integer, cursor++, 1, Tri::Set, Tri::Reset);

#if defined BOARD_Tamu_v2_0
    // 5. Name {1} - Depth 0
    // This is a sibling of the Root System node {0}.
    Values.Set(Name.Data, Name.Length, Types::Text, cursor++, 0, Tri::Reset, Tri::Set);
#endif

    // Note: UpdateSkip() is called inside Set() automatically,
    // but since we filled it linearly, the math is extremely fast.
}

bool BoardClass::Run()
{
    // Anchor Discovery
    uint16_t portsIdx = Values.Child(0); 
    uint16_t bootTimeIdx = Values.Next(portsIdx); 

    // Indices relative to the anchor
    uint16_t avgIdx = bootTimeIdx + 1;
    uint16_t maxIdx = bootTimeIdx + 2;

    Result avgRes = Values.Get(avgIdx);
    Result maxRes = Values.Get(maxIdx);

    if (avgRes.Value && maxRes.Value)
    {
        // Use references to the Number class instances in the ValueTree
        Number &avg = *(Number *)avgRes.Value;
        Number &max = *(Number *)maxRes.Value;

        // avg = (avg * 15 + current) / 16
        avg = (avg * 0.9375) + (Number(DeltaTime) * 0.0625);

        if (Number(DeltaTime) > max)
        {
            max = Number(DeltaTime);
        }
        else if (HW::Now() % 20000 < 20)
        {
            max = avg; // Reset max to current avg for long-term windowing

            // Update RAM Telemetry (bootTimeIdx + 3 and + 4)
            int32_t totalRam = HW::GetRAM();
            int32_t freeRam = HW::GetFreeRAM();
            Values.SetExisting(&totalRam, 4, Types::Integer, bootTimeIdx + 3);
            Values.SetExisting(&freeRam, 4, Types::Integer, bootTimeIdx + 4);
        }
    }

    // 3. Driver Servicing Loop (Structural Navigation)
    uint16_t currentPort = Values.Child(portsIdx);
    for (uint16_t i = 0; i < PORT_COUNT; i++)
    {
        if (currentPort == INVALID_HEADER) break;

        if (DriverArray[i] != nullptr)
        {
            // Standard Driver structure: Port -> Pin -> Driver
            uint16_t pinIdx = Values.Child(currentPort);
            uint16_t roleIdx = Values.Next(pinIdx);

            Result role = Values.Get(roleIdx);
            if (role.Value && *(Drivers *)role.Value == Drivers::LED)
            {
                static_cast<LEDDriver *>(DriverArray[i])->Show();
            }
        }
        currentPort = Values.Next(currentPort);
    }

    return true;
}