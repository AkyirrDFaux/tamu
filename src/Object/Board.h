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

#define SET_PORT(index, type, pin, driver)                                      \
    do                                                                          \
    {                                                                           \
        PortTypeClass pt(type);                                                 \
        Values.Set(&pt, sizeof(PortTypeClass), Types::PortType, {0, 0, index}); \
        Pin p(pin);                                                             \
        Values.Set(&p, sizeof(Pin), Types::Pin, {0, 0, index, 0});              \
        Drivers d = driver;                                                     \
        Values.Set(&d, sizeof(Drivers), Types::PortDriver, {0, 0, index, 1});   \
    } while (0)

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
    Values.Set(&model, sizeof(Boards), Types::Board, {0});

    SET_PORT(0, Ports::GPIO | Ports::ADC | Ports::PWM, 0, Drivers::None);
    SET_PORT(1, Ports::GPIO | Ports::ADC | Ports::PWM, 1, Drivers::None);
    SET_PORT(2, Ports::GPIO | Ports::PWM, 9, Drivers::None);
    SET_PORT(3, Ports::TOut | Ports::PWM, 10, Drivers::None);
    SET_PORT(4, Ports::TOut | Ports::PWM, 6, Drivers::None);
    SET_PORT(5, Ports::GPIO | Ports::PWM, 8, Drivers::None);
    SET_PORT(6, Ports::GPIO | Ports::PWM, 7, Drivers::None);
    SET_PORT(7, Ports::GPIO | Ports::ADC | Ports::PWM, 3, Drivers::None);
    SET_PORT(8, Ports::I2C_SDA, 4, Drivers::None);
    SET_PORT(9, Ports::I2C_SCL, 5, Drivers::None);
    SET_PORT(10, Ports::GPIO | Ports::Internal, LED_NOTIFICATION_PIN, Drivers::None);
#endif

    // Standard Board Values (Local Paths)
    int32_t zero = 0;
    Values.Set(&zero, sizeof(int32_t), Types::Integer, {0, 1}); // Boot Time
    Values.Set(&zero, sizeof(int32_t), Types::Integer, {0, 2}); // Avg Loop
    Values.Set(&zero, sizeof(int32_t), Types::Integer, {0, 3}); // Max Loop
    Values.Set(&zero, sizeof(int32_t), Types::Integer, {0, 4}); // RAM Total
    Values.Set(&zero, sizeof(int32_t), Types::Integer, {0, 5}); // RAM Free

    Values.Set(Name.Data, Name.Length, Types::Text, {1});
};

bool BoardClass::Run()
{
    // 1. Update Telemetry using local SearchResults
    SearchResult avgRes = Values.Find({0, 2}, true);
    SearchResult maxRes = Values.Find({0, 3}, true);

    if (avgRes.Value && maxRes.Value)
    {
        int32_t *avg = (int32_t *)avgRes.Value;
        int32_t *max = (int32_t *)maxRes.Value;

        // Exponential moving average for loop timing
        *avg = (DeltaTime + (*avg * 15)) / 16;

        if (DeltaTime > *max)
        {
            *max = DeltaTime;
        }
        else if (HW::Now() % 20000 < 20) // Periodic update of RAM stats
        {
            *max = *avg; // Reset max peak occasionally
            int32_t totalRam = HW::GetRAM();
            int32_t freeRam = HW::GetFreeRAM();
            Values.Set(&totalRam, sizeof(int32_t), Types::Integer, {0, 4});
            Values.Set(&freeRam, sizeof(int32_t), Types::Integer, {0, 5});
        }
    }

    // 2. Driver Servicing
    for (uint8_t i = 0; i < 11; i++)
    {
        if (DriverArray[i] == nullptr)
            continue;

        SearchResult roleRes = Values.Find({0, 0, i, 1}, true);
        if (roleRes.Value && *(Drivers *)roleRes.Value == Drivers::LED)
        {
            static_cast<LEDDriver *>(DriverArray[i])->Show();
        }
    }
    return true;
}