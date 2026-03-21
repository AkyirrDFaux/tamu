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
    BoardClass(const Reference &ID);
    void Setup(Path Index);
    bool Run(); // For LED outputs
    void UpdateLoopTime();

    void PortSetup(uint8_t Port);
    void PortReset(BaseClass *Object);
    bool Connect(BaseClass *Object, uint8_t Port, uint8_t Index = 0);
    bool Disconnect(BaseClass *Object, uint8_t Port);

    static void SetupBridge(BaseClass *Base, Path Index) { static_cast<BoardClass *>(Base)->Setup(Index); }
    static bool RunBridge(BaseClass *Base) { return static_cast<BoardClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = BoardClass::SetupBridge,
        .Run = BoardClass::RunBridge};
};

constexpr VTable BoardClass::Table;

BoardClass::BoardClass(const Reference &ID) : BaseClass(&Table, ID, {Flags::Auto, 1})
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

bool BoardClass::Connect(BaseClass *Object, uint8_t Port, uint8_t Index)
{
    ESP_LOGI("Board", "- Connect(Port: %d, Idx: %d)\n", Port, Index);
    if (Object == nullptr || Port < 0 || Port > 10)
        return false;

    Reference ID = Objects.GetReference(Object);
    Getter<Drivers> Driver = Values.Get<Drivers>({1, (uint8_t)Port, 1});
    if (ID == Reference(0, 0, 0) || !Driver.Success)
        return false;

    // 1. Bus Logic: If it's I2C, ignore the passed Index and find the first hole
    if (Driver == Drivers::I2C_SCL || Driver == Drivers::I2C_SDA)
    {
        uint8_t Search = 0;
        while (Search < 32)
        {
            Getter<Reference> Entry = Values.Get<Reference>({1, Port, 1, Search});
            if (!Entry.Success)
            {
                Index = Search;
                break;
            }
            if (Entry.Value == ID)
                return false; // Already here
            Search++;
        }
    }
    else
    {
        // 2. Direct/Chain Logic: Strict Indexing
        // If no driver is set, we only allow raw connection at Index 0
        if (Driver.Value == Drivers::None && Index != 0)
            return false;

        // Overwrite Protection: Block if this specific slot is taken
        if (Values.Get<Reference>({1, Port, 1, Index}).Success)
            return false;
    }

    Values.Set<Reference>(ID, {1, Port, 1, Index});
    PortSetup(Port);
    return true;
}

bool BoardClass::Disconnect(BaseClass *Object, uint8_t Port)
{
    if (Object == nullptr || Port > 10)
        return false;

    Reference ID = Objects.GetReference(Object);
    Getter<Drivers> Driver = Values.Get<Drivers>({1, Port, 1});

    // If the object reference is invalid or no driver exists on this port,
    // there is nothing to disconnect.
    if (ID == Reference(0, 0, 0) || !Driver.Success)
        return false;

    uint8_t Slot = 0;
    while (Slot < 32)
    {
        Path SlotPath = {1, Port, 1, Slot};
        Getter<Reference> Entry = Values.Get<Reference>(SlotPath);

        if (!Entry.Success)
            break; // Reached the end of the current driver's list

        if (Entry.Value == ID)
        {
            // --- FOUND THE OBJECT ---
            PortReset(Object);
            // 1. Bus Logic: If it's I2C, perform a Shift-Left to keep it packed
            if (Driver == Drivers::I2C_SCL || Driver == Drivers::I2C_SDA)
            {
                Values.Delete(SlotPath);
                uint8_t Next = Slot + 1;
                while (Next < 32)
                {
                    Getter<Reference> Upstream = Values.Get<Reference>({1, Port, 1, Next});
                    if (!Upstream.Success)
                        break;

                    // Move upstream reference down and clear the old index
                    Values.Set<Reference>(Upstream.Value, {1, Port, 1, (uint8_t)(Next - 1)});
                    Values.Delete({1, Port, 1, Next});
                    Next++;
                }
            }
            else
            {
                // 2. Chain/Direct/None Logic: Wipe-Upstream (Scorched Earth)
                // Deleting the target and everything following it in the chain
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
    return false; // Object not found on this port
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

    for (uint8_t i = 0; i < 11; i++)
    {
        if (DriverArray[i] != nullptr)
        {
            // We know from PortSetup that for now, these are LEDDrivers
            LEDDriver *Driver = static_cast<LEDDriver *>(DriverArray[i]);
            Driver->Show();
        }
    }
    return true;
}