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

// Updated Macro to use the new Reference system
#define SET_PORT(index, type, pin, driver)            \
    do                                                \
    {                                                 \
        Values.Set(PortTypeClass(type), {1, index});  \
        Values.Set(Pin(pin), {1, index, 0});          \
        Values.Set(driver, {1, index, 1});            \
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

    void DriverStop(uint8_t Port);
    void DriverStart(uint8_t Port);
    void PortSetup(uint8_t Port);
    void PortReset(BaseClass *Object);

    bool Connect(BaseClass *Object, uint8_t Port, uint8_t Index = 0);
    bool Disconnect(BaseClass *Object, uint8_t Port);

    // Bridge functions updated for Reference
    static void SetupBridge(BaseClass *Base, const Reference &Index) { 
        static_cast<BoardClass *>(Base)->Setup(Index); 
    }
    static bool RunBridge(BaseClass *Base) { 
        return static_cast<BoardClass *>(Base)->Run(); 
    }
    
    static constexpr VTable Table = {
        .Setup = BoardClass::SetupBridge,
        .Run = BoardClass::RunBridge
    };
};

BoardClass::BoardClass(const Reference &ID) : BaseClass(&Table, ID, {Flags::Auto, 1})
{
    Type = ObjectTypes::Board;
    Name = "Board";

    // Standard Board Values (Local Paths)
    Values.Set(Text("Unnamed"), {0, 0});
    Values.Set((int32_t)0,      {0, 1});
    Values.Set((int32_t)0,      {0, 2});
    Values.Set((int32_t)0,      {0, 3});
    Values.Set((int32_t)0,      {0, 4});
    Values.Set((int32_t)0,      {0, 5});

#if defined BOARD_Tamu_v2_0
    Values.Set(Boards::Tamu_v2_0, {0});

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
};

bool BoardClass::Connect(BaseClass *Object, uint8_t Port, uint8_t Index)
{
    if (Object == nullptr || Port > 10) return false;

    Reference ID = Objects.GetReference(Object);
    Getter<Drivers> Driver = Values.Get<Drivers>({1, Port, 1});
    
    if (ID == Reference() || !Driver.Success) return false;

    if (Driver == Drivers::I2C_SCL || Driver == Drivers::I2C_SDA)
    {
        uint8_t Search = 0;
        while (Search < 32)
        {
            Getter<Reference> Entry = Values.Get<Reference>({1, Port, 1, Search});
            if (!Entry.Success) {
                Index = Search;
                break;
            }
            if (Entry.Value == ID) return false; 
            Search++;
        }
    }
    else
    {
        if (Driver.Value == Drivers::None && Index != 0) return false;
        if (Values.Get<Reference>({1, Port, 1, Index}).Success) return false;
    }

    Values.Set<Reference>(ID, {1, Port, 1, Index});
    PortSetup(Port);
    return true;
}

bool BoardClass::Disconnect(BaseClass *Object, uint8_t Port)
{
    if (Object == nullptr || Port > 10) return false;

    Reference ID = Objects.GetReference(Object);
    Getter<Drivers> Driver = Values.Get<Drivers>({1, Port, 1});

    if (ID == Reference() || !Driver.Success) return false;

    uint8_t Slot = 0;
    while (Slot < 32)
    {
        Reference SlotPath = {1, Port, 1, Slot};
        Getter<Reference> Entry = Values.Get<Reference>(SlotPath);

        if (!Entry.Success) break;

        if (Entry.Value == ID)
        {
            PortReset(Object);
            if (Driver == Drivers::I2C_SCL || Driver == Drivers::I2C_SDA)
            {
                Values.Delete(SlotPath);
                uint8_t Next = Slot + 1;
                while (Next < 32)
                {
                    Getter<Reference> Upstream = Values.Get<Reference>({1, Port, 1, Next});
                    if (!Upstream.Success) break;

                    Values.Set<Reference>(Upstream.Value, {1, Port, 1, (uint8_t)(Next - 1)});
                    Values.Delete({1, Port, 1, Next});
                    Next++;
                }
            }
            else
            {
                uint8_t ToWipe = Slot;
                while (Values.Get<Reference>({1, Port, 1, ToWipe}).Success)
                {
                    Values.Delete({1, Port, 1, ToWipe});
                    ToWipe++;
                }
            }
            PortSetup(Port);
            return true;
        }
        Slot++;
    }
    return false;
}

bool BoardClass::Run()
{
    // Simplified ValueGet calls using Reference
    int32_t AvgLoopTime = ValueGet<int32_t>({0, 2}).Value;

    Values.Set<int32_t>((DeltaTime + AvgLoopTime * 15) / 16, {0, 2});
    
    if (DeltaTime > ValueGet<int32_t>({0, 3}).Value)
        Values.Set<int32_t>(DeltaTime, {0, 3});
    else if (HW::Now() % 20000 < 20)
    {
        Values.Set<int32_t>(AvgLoopTime, {0, 3});
        Values.Set<int32_t>(HW::GetRAM(), {0, 4});
        Values.Set<int32_t>(HW::GetFreeRAM(), {0, 5});
    }

    for (uint8_t i = 0; i < 11; i++)
    {
        if (DriverArray[i] == nullptr) continue;

        Getter<Drivers> Role = Values.Get<Drivers>({1, i, 1});
        if (Role.Success && Role.Value == Drivers::LED)
        {
            static_cast<LEDDriver*>(DriverArray[i])->Show();
        }
    }
    return true;
}