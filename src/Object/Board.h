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

struct PortMap {
    uint16_t Port;   // Index of {0, 0, Port}
    uint16_t Driver; // Index of {0, 0, Port, 1}
    uint16_t Ref;    // Index of {0, 0, Port, 1, 0}
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
    
    // 1. Root node (Index 0).
    Values.Set(&model, sizeof(Boards), Types::Board, 0, true);

    // 2. "Ports" container {0, 0}
    uint16_t ports = Values.InsertChild(nullptr, 0, Types::Undefined, 0, true);

    uint16_t lastPort = 0; 
    for (uint8_t i = 0; i < (sizeof(Tamu_Ports) / sizeof(PortDefinition)); i++)
    {
        const PortDefinition &p = Tamu_Ports[i];

        // 3. Port Type {0, 0, i}
        uint16_t pNode = (i == 0) 
            ? Values.InsertChild(&p.Type, sizeof(PortTypeClass), Types::PortType, ports, true)
            : Values.InsertNext(&p.Type, sizeof(PortTypeClass), Types::PortType, lastPort, true);

        // 4. Pin {0, 0, i, 0}
        uint16_t pin = Values.InsertChild(&p.PinID, sizeof(Pin), Types::Pin, pNode, true);

        // 5. Driver {0, 0, i, 1}
        Values.InsertNext(&p.Driver, sizeof(Drivers), Types::PortDriver, pin, true);

        lastPort = pNode; 
    }

    // 6. Telemetry: {0, 1} through {0, 5}
    int32_t zero = 0;
    uint16_t lastTele = ports;
    for (uint8_t i = 0; i < 5; i++)
    {
        lastTele = Values.InsertNext(&zero, 4, Types::Integer, lastTele, true);
    }

    // 7. Name {1} (Sibling of system {0})
    Values.InsertNext(Name.Data, Name.Length, Types::Text, 0, false, true);
#endif
}

bool BoardClass::Run()
{
    // 1. Fast Navigation via Indices
    // We know 'System' is index 0. If you wanted to be safe, you'd use Find.
    uint16_t systemIdx = 0; 
    
    // Grab Telemetry indices using lean Next/Child
    uint16_t portsIdx    = Values.Child(systemIdx);
    uint16_t bootTimeIdx = Values.Next(portsIdx);
    uint16_t avgIdx      = Values.Next(bootTimeIdx);
    uint16_t maxIdx      = Values.Next(avgIdx);
    uint16_t ramTotalIdx = Values.Next(maxIdx);
    uint16_t ramFreeIdx  = Values.Next(ramTotalIdx);

    // Resolve values for math
    Result avgRes = Values.Get(avgIdx);
    Result maxRes = Values.Get(maxIdx);

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
            
            // Using the updated Set(index) which handles direct writes
            Values.Set(&totalRam, 4, Types::Integer, ramTotalIdx, true);
            Values.Set(&freeRam, 4, Types::Integer, ramFreeIdx, true);
        }
    }

    // 2. Driver Servicing Loop
    // Navigate: System -> Ports -> First Port
    uint16_t currentPort = Values.Child(portsIdx);

    for (uint8_t i = 0; i < 11; i++)
    {
        if (currentPort == 0xFFFF) break;

        if (DriverArray[i] != nullptr)
        {
            // Driver is at {0, 0, i, 1}
            // Logic: currentPort -> Child (Pin) -> Next (Driver)
            uint16_t pinIdx = Values.Child(currentPort);
            uint16_t roleIdx = Values.Next(pinIdx);
            
            Result role = Values.Get(roleIdx);

            if (role.Value && *(Drivers *)role.Value == Drivers::LED)
            {
                static_cast<LEDDriver *>(DriverArray[i])->Show();
            }
        }
        
        // Move to next port sibling
        currentPort = Values.Next(currentPort);
    }

    return true;
}