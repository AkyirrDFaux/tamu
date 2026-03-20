class PortTypeClass
{
public:
    uint32_t Values = Ports::None;
    PortTypeClass() {};
    PortTypeClass(uint32_t Input) { Values = Input; };
    void operator=(uint32_t Other) { Values = Other; };
    void operator+=(uint32_t Other) { Values |= Other; };
    void operator-=(uint32_t Other) { Values &= ~Other; };
    bool operator==(Ports::Ports Other) { return Values & Other; };
    bool operator!=(Ports::Ports Other) { return !(Values & Other); };
};

#define SET_PORT(index, type, pin, driver)           \
    do                                               \
    {                                                \
        Values.Set(PortTypeClass(type), {1, index}); \
        Values.Set(Pin(pin), {1, index, 0});         \
        Values.Set(driver, {1, index, 1});           \
    } while (0)

class BoardClass : public BaseClass
{
public:
    void *DriverArray[11] = {nullptr};
    BoardClass(Reference ID);
    void Setup(Path Index);
    bool Run(); // For LED outputs
    void UpdateLoopTime();

    void RebuildChain(int32_t Port);
    bool Connect(BaseClass *Object, int32_t Index = 0);
    bool Disconnect(BaseClass *Object);

    static void SetupBridge(BaseClass *Base, Path Index) { static_cast<BoardClass *>(Base)->Setup(Index); }
    static bool RunBridge(BaseClass *Base) { return static_cast<BoardClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = BoardClass::SetupBridge,
        .Run = BoardClass::RunBridge};
};

constexpr VTable BoardClass::Table;

BoardClass::BoardClass(Reference ID) : BaseClass(&Table, ID, Flags::Auto) // Board always loads first, it takes ID 1
{
    Type = ObjectTypes::Board;
    Name = "Board";

    Values.Set(Text("Unnamed"), {0, 0});
    Values.Set((int32_t)0, {0, 1});
    Values.Set((int32_t)0, {0, 2});
    Values.Set((int32_t)0, {0, 3});
    Values.Set((int32_t)0, {0, 4});
    Values.Set((int32_t)0, {0, 5});

#if defined BOARD_Tamu_v1_0
    *Values.At<Boards>(0) = Boards::Tamu_v1_0;
    AddModule(new PortClass(15, Ports::GPIO), 0); // J1
    AddModule(new PortClass(13, Ports::GPIO), 1); // J2
    AddModule(new PortClass(14, Ports::GPIO), 2); // J5
    AddModule(new PortClass(2, Ports::GPIO), 3);  // J6
    AddModule(new PortClass(12, Ports::TOut), 4); // J3
#elif defined BOARD_Tamu_v2_0
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

    /*AddModule(new I2CClass(RandomID, Flags::Auto), 11);
    Modules[11]->AddModule(Modules[8], 0);
    Modules[11]->AddModule(Modules[9], 1);

    AddModule(new GyrAccClass(RandomID, Flags::Auto), 12);
    Modules[12]->ValueSet<GyrAccs>(GyrAccs::LSM6DS3TRC, 0);
    Modules[11]->AddModule(Modules[12], 2);

    AddModule(new InputClass(RandomID, Flags::Auto), 13);
    Modules[13]->ValueSet<Inputs>(Inputs::ButtonWithLED, 0);
    Modules[10]->AddModule(Modules[13], 0);*/
#elif defined BOARD_Valu_v2_0
    ValueSet(Boards::Valu_v2_0, BoardType);
    AddModule(new PortClass({GPIOA, 14}, Ports::GPIO | Ports::PWM), 0);             // L1
    AddModule(new PortClass({GPIOB, 8}, Ports::GPIO | Ports::PWM), 1);              // L2
    AddModule(new PortClass({GPIOB, 10}, Ports::GPIO), 2);                          // L3
    AddModule(new PortClass({GPIOB, 11}, Ports::GPIO), 3);                          // L4
    AddModule(new PortClass({GPIOB, 1}, Ports::GPIO | Ports::PWM | Ports::ADC), 4); // L5
    AddModule(new PortClass({GPIOA, 8}, Ports::TOut), 5);                           // F

    AddModule(new PortClass({GPIOA, 6}, Ports::GPIO | Ports::ADC), 6); // S1
    AddModule(new PortClass({GPIOA, 1}, Ports::GPIO | Ports::ADC), 7); // S2
    AddModule(new PortClass({GPIOA, 0}, Ports::GPIO | Ports::ADC), 8); // S3

    AddModule(new PortClass({GPIOA, 10}, Ports::UART_RX), 9);
    AddModule(new PortClass({GPIOA, 9}, Ports::UART_TX), 10);
    AddModule(new PortClass({GPIOB, 6}, Ports::I2C_SCL), 11);
    AddModule(new PortClass({GPIOB, 7}, Ports::I2C_SDA), 12);

    AddModule(new PortClass({GPIOB, 15}, Ports::GPIO | Ports::Internal), 13);          // B1
    AddModule(new PortClass({GPIOB, 14}, Ports::GPIO | Ports::Internal), 14);          // B2
    AddModule(new PortClass({GPIOB, 13}, Ports::GPIO | Ports::Internal), 15);          // B3
    AddModule(new PortClass(LED_NOTIFICATION_PIN, Ports::GPIO | Ports::Internal), 16); // B4 + LED

    AddModule(new InputClass(), 17);
    Modules[17]->ValueSet<Inputs>(Inputs::Button, 0);
    Modules[13]->AddModule(Modules[17], 0);
    AddModule(new InputClass(), 18);
    Modules[18]->ValueSet<Inputs>(Inputs::Button, 0);
    Modules[14]->AddModule(Modules[18], 0);
    AddModule(new InputClass(), 19);
    Modules[19]->ValueSet<Inputs>(Inputs::Button, 0);
    Modules[15]->AddModule(Modules[19], 0);
    AddModule(new InputClass(), 20);
    Modules[20]->ValueSet<Inputs>(Inputs::ButtonWithLED, 0);
    Modules[16]->AddModule(Modules[20], 0);

    AddModule(new OLEDClass(), 21);
    Modules[21]->AddModule(Modules[19], 0);
    Modules[21]->AddModule(Modules[18], 1);
    Modules[21]->AddModule(Modules[17], 2);
    Modules[21]->AddModule(Modules[20], 3);

    /*AddModule(new PortClass({GPIOA, 5}, Ports::SPI_CLK), 17);
    AddModule(new PortClass({GPIOA, 7}, Ports::SPI_MOSI), 18);
    AddModule(new PortClass({GPIOA, 3}, Ports::SPI_DRST), 19);
    AddModule(new PortClass({GPIOB, 0}, Ports::SPI_DDC), 20);
    AddModule(new PortClass({GPIOA, 4}, Ports::SPI_CS), 21);*/
#endif
};

void BoardClass::Setup(Path Index) {
};

bool BoardClass::Connect(BaseClass *Object, int32_t Index)
{
    if (Object == nullptr)
        return false;

    // 1. Find the local ID (Index) of the object in the global registry
    int32_t ObjectIdx = Objects.Search(Object);
    if (ObjectIdx == -1)
        return false;

    // Create the Reference using the found index (Net=0, Group=0, Device=ObjectIdx)
    // Adjust N/G/D mapping based on your specific network architecture
    Reference ObjectRef(0, 0, (uint8_t)ObjectIdx);

    // 2. Find the Port by walking up to the Board
    BaseClass *Current = Object;
    while (true)
    {
        Getter<Reference> Link = Current->Values.Get<Reference>({0, 0});
        if (!Link.Success)
            return false;

        BaseClass *Upstream = Objects.At(Link.Value);
        if (Upstream != nullptr && Upstream->Type == ObjectTypes::Board)
        {
            int32_t Port = (int32_t)Link.Value.Location.Indexing[1];

            // 3. Register the Reference into the Board's database {1, Port, 2...}
            // We use 'Index' passed from the peripheral to determine the slot
            // so the order is preserved (Board <- 0 <- 1 <- 2)
            uint8_t slot = (uint8_t)(2 + Index);
            Values.Set<Reference>(ObjectRef, {1, (uint8_t)Port, slot});

            RebuildChain(Port);
            return true;
        }
        Current = Upstream;
        if (Current == nullptr)
            break;
    }
    return false;
}

bool BoardClass::Disconnect(BaseClass *Object)
{
    if (Object == nullptr)
        return false;

    // 1. Identify the object's unique Reference via the Registry Search
    int32_t ObjectIdx = Objects.Search(Object);
    if (ObjectIdx == -1)
        return false;

    Reference TargetRef(0, 0, (uint8_t)ObjectIdx);

    // 2. Scan all ports to find where this Reference is stored
    for (uint8_t p = 0; p < 11; p++)
    {
        uint8_t slot = 2;
        bool found = false;

        while (true)
        {
            Getter<Reference> CurrentSlot = Values.Get<Reference>({1, p, slot});
            if (!CurrentSlot.Success)
                break; // End of this port's list

            if (CurrentSlot.Value == TargetRef)
            {
                // 3. Found the object. Remove it from the Board's database.
                Values.Delete({1, p, slot});
                found = true;

                // 4. Defragment the list: Shift subsequent devices down
                // This ensures the Registry {2, 3, 4...} remains contiguous
                uint8_t nextSlot = slot + 1;
                while (true)
                {
                    Getter<Reference> NextRef = Values.Get<Reference>({1, p, nextSlot});
                    if (!NextRef.Success)
                        break;

                    // Move the next reference into the current (now empty) slot
                    Values.Set<Reference>(NextRef.Value, {1, p, (uint8_t)(nextSlot - 1)});
                    Values.Delete({1, p, nextSlot});
                    nextSlot++;
                }
                break;
            }
            slot++;
        }

        if (found)
        {
            // 5. Trigger a Rebuild for this port to update the remaining
            // devices' LEDs pointers and the total driver length.
            RebuildChain(p);
            return true;
        }
    }

    return false;
}

bool BoardClass::Run()
{
    Getter<int32_t> AvgLoopTime = ValueGet<int32_t>({0, 2});

    Values.Set<int32_t>((DeltaTime + AvgLoopTime * 15) / 16, {0, 2});
    if (DeltaTime > ValueGet<int32_t>({0, 3}))
        Values.Set<int32_t>(DeltaTime, {0, 3});
    else if (HW::Now() % 20000 < 20)
    {
        Values.Set<int32_t>(AvgLoopTime, {0, 3});
        Values.Set<int32_t>(HW::GetRAM(), {0, 4});
        Values.Set<int32_t>(HW::GetFreeRAM(), {0, 5});
    }
    return true;
}