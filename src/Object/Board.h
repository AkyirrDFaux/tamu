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

#if defined BOARD_Tamu_v2_0
static const PortDefinition Tamu_Ports[] = {
    {Ports::GPIO | Ports::ADC | Ports::PWM, {0,0}, Drivers::None},
    {Ports::GPIO | Ports::ADC | Ports::PWM, {1,0}, Drivers::None},
    {Ports::GPIO | Ports::PWM, {9,0}, Drivers::None},
    {Ports::TOut | Ports::PWM, {10,0}, Drivers::None},
    {Ports::TOut | Ports::PWM, {6,0}, Drivers::None},
    {Ports::GPIO | Ports::PWM, {8,0}, Drivers::None},
    {Ports::GPIO | Ports::PWM, {7,0}, Drivers::None},
    {Ports::GPIO | Ports::ADC | Ports::PWM, {3,0}, Drivers::None},
    {Ports::I2C_SDA, {4,0}, Drivers::None},
    {Ports::I2C_SCL, {5,0}, Drivers::None},
    {Ports::GPIO | Ports::Internal, LED_NOTIFICATION_PIN, Drivers::None}};
#endif

class BoardClass : public BaseClass
{
public:
    void *DriverArray[11] = {nullptr};
    BoardClass(const Reference &ID);

    // Updated to use Reference
    void Setup(const Reference &Index);
    bool Run();
    void UpdateLoopTime();

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
    static void SetupBridge(BaseClass *Base, const Reference &Index)
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
    
    // 1. Root node (Index 0). Passing 0xFFFF as parent signals root.
    uint16_t system = Values.InsertChild(&model, sizeof(Boards), Types::Board, 0xFFFF);

    // 2. "Ports" container {0, 0}
    uint16_t ports = Values.InsertChild(nullptr, 0, Types::Undefined, system);

    uint16_t lastPort = 0; 
    for (uint8_t i = 0; i < (sizeof(Tamu_Ports) / sizeof(PortDefinition)); i++)
    {
        const PortDefinition &p = Tamu_Ports[i];

        // 3. Port Type {0, 0, i}
        uint16_t pNode = (i == 0) 
            ? Values.InsertChild(&p.Type, sizeof(PortTypeClass), Types::PortType, ports)
            : Values.InsertNext(&p.Type, sizeof(PortTypeClass), Types::PortType, lastPort);

        // 4. Pin {0, 0, i, 0}
        uint16_t pin = Values.InsertChild(&p.PinID, sizeof(Pin), Types::Pin, pNode);

        // 5. Driver {0, 0, i, 1}
        Values.InsertNext(&p.Driver, sizeof(Drivers), Types::PortDriver, pin);

        lastPort = pNode; 
    }

    // 6. Telemetry: {0, 1} through {0, 5} (Siblings of ports container)
    int32_t zero = 0;
    uint16_t lastTele = ports;
    for (uint8_t i = 0; i < 5; i++)
    {
        lastTele = Values.InsertNext(&zero, 4, Types::Integer, lastTele);
    }

    // 7. Name {1} (Sibling of system {0})
    Values.InsertNext(Name.Data, Name.Length, Types::Text, system);
#endif
}
bool BoardClass::Run()
{
    // 1. Navigation: Navigate to the "System" node (0)
    SearchResult system = Values.Find({0}, true);
    if (!system.Value) return false;

    // Grab bookmarks for Telemetry (Siblings of the first child of System)
    // Path {0, 1} is usually the first child
    SearchResult bootTime = Values.Child(system.Location);
    SearchResult avgRes   = Values.Next(bootTime.Location); // {0, 2}
    SearchResult maxRes   = Values.Next(avgRes.Location);   // {0, 3}
    SearchResult ramTotal = Values.Next(maxRes.Location);   // {0, 4}
    SearchResult ramFree  = Values.Next(ramTotal.Location); // {0, 5}

    if (avgRes.Value && maxRes.Value)
    {
        int32_t *avg = (int32_t *)avgRes.Value;
        int32_t *max = (int32_t *)maxRes.Value;

        *avg = (DeltaTime + (*avg * 15)) / 16;

        if (DeltaTime > *max) {
            *max = DeltaTime;
        }
        else if (HW::Now() % 20000 < 20) {
            *max = *avg;
            int32_t totalRam = HW::GetRAM();
            int32_t freeRam = HW::GetFreeRAM();
            
            // SetDirect is much faster here than Values.Set with a Reference
            Values.SetDirect(&totalRam, 4, Types::Integer, ramTotal.Location);
            Values.SetDirect(&freeRam, 4, Types::Integer, ramFree.Location);
        }
    }

    // 2. Optimized Driver Servicing
    // Navigate to the Ports array: root -> child (0:System) -> child (0, 0:Ports)
    SearchResult portsNode = Values.Child(system.Location); // This is {0, 0}
    SearchResult currentPort = Values.Child(portsNode.Location); // This is {0, 0, 0}

    for (uint8_t i = 0; i < 11; i++)
    {
        if (DriverArray[i] != nullptr && currentPort.Value)
        {
            // The Driver is at {0, 0, i, 1}. 
            // In the tree: PortNode -> Child (Pin: 0) -> Next (Driver: 1)
            SearchResult pinNode = Values.Child(currentPort.Location);
            SearchResult roleRes = Values.Next(pinNode.Location);

            if (roleRes.Value && *(Drivers *)roleRes.Value == Drivers::LED)
            {
                static_cast<LEDDriver *>(DriverArray[i])->Show();
            }
        }
        
        // Move to the next port sibling {0, 0, i+1}
        currentPort = Values.Next(currentPort.Location);
        if (!currentPort.Value) break; 
    }

    return true;
}