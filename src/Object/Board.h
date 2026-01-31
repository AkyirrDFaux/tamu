class BoardClass : public BaseClass
{
public:
    enum Value
    {
        BoardType,
        BTName,
        BootTime,
        AvgTime,
        MaxTime,
        MemoryTotal,
        MemoryUsed
    };

    BoardClass(IDClass ID = (1 << 8), FlagClass Flags = Flags::None);
    void Setup(int32_t Index = -1);
    void UpdateLoopTime();
};

BoardClass::BoardClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags) // Board always loads first, it takes ID 1
{
    Type = ObjectTypes::Board;
    Name = "Board";

    ValueSet(Boards::Undefined);
    ValueSet(String("Unnamed"));
    ValueSet((uint32_t)0, BootTime);
    ValueSet((uint32_t)0, AvgTime);
    ValueSet((uint32_t)0, MaxTime);
    ValueSet((int32_t)0, MemoryTotal);
    ValueSet((int32_t)0, MemoryUsed);
};

void BoardClass::Setup(int32_t Index) // Load Presets
{
    if (Index == 0 && Values.Type(0) == Types::Board && ValueGet<Boards>(0) == Boards::Undefined) // Double setup prevention
    {
#ifdef BOARD_Tamu_v1_0
        *Values.At<Boards>(0) = Boards::Tamu_v1_0;
        AddModule(new PortClass(15, Ports::GPIO), 0); // J1
        AddModule(new PortClass(13, Ports::GPIO), 1); // J2
        AddModule(new PortClass(14, Ports::GPIO), 2); // J5
        AddModule(new PortClass(2, Ports::GPIO), 3);  // J6
        AddModule(new PortClass(12, Ports::TOut), 4); // J3
#endif
#ifdef BOARD_Tamu_v2_0
        ESP32PWM::allocateTimer(0);
        ESP32PWM::allocateTimer(1);
        ESP32PWM::allocateTimer(2);
        ESP32PWM::allocateTimer(3);
        ValueSet(Boards::Tamu_v2_0, BoardType);
        AddModule(new PortClass(0, Ports::GPIO | Ports::ADC | Ports::PWM), 0); // P1
        AddModule(new PortClass(1, Ports::GPIO | Ports::ADC | Ports::PWM), 1); // P2
        AddModule(new PortClass(9, Ports::GPIO | Ports::PWM), 2);              // P3 - S, no ADC!
        AddModule(new PortClass(10, Ports::TOut | Ports::PWM), 3);             // P4 - F !!! Notation of switch is inverted

        AddModule(new PortClass(6, Ports::TOut | Ports::PWM), 4);              // P5 - F
        AddModule(new PortClass(8, Ports::GPIO | Ports::PWM), 5);              // P6 - S , no ADC!
        AddModule(new PortClass(7, Ports::GPIO | Ports::PWM), 6);              // P7 , no ADC!
        AddModule(new PortClass(3, Ports::GPIO | Ports::ADC | Ports::PWM), 7); // P8

        AddModule(new PortClass(4, Ports::I2C_SDA), 8);
        AddModule(new PortClass(5, Ports::I2C_SCL), 9);
        AddModule(new PortClass(LED_NOTIFICATION_PIN, Ports::GPIO | Ports::Internal), 10);

        AddModule(new I2CClass(), 11);
        Modules[11]->AddModule(Modules[8], 0);
        Modules[11]->AddModule(Modules[9], 1);

        AddModule(new GyrAccClass(), 12);
        Modules[12]->ValueSet<GyrAccs>(GyrAccs::LSM6DS3TRC, 0);
        Modules[11]->AddModule(Modules[12], 2);

        AddModule(new InputClass(), 13);
        Modules[13]->ValueSet<Inputs>(Inputs::ButtonWithLED, 0);
        Modules[10]->AddModule(Modules[13], 0);
#endif
    }
};

void BoardClass::UpdateLoopTime()
{
    uint32_t AvgLoopTime = ValueGet<uint32_t>(AvgTime);

    
    ValueSet<uint32_t>((DeltaTime + AvgLoopTime * 15) / 16, AvgTime);
    if (DeltaTime > ValueGet<uint32_t>(MaxTime))
        ValueSet<uint32_t>(DeltaTime, MaxTime);
    else if (millis() % 20000 < 20)
    {
        ValueSet<uint32_t>(AvgLoopTime, MaxTime);
        ValueSet<int32_t>(GetHeap(), MemoryTotal);
        ValueSet<int32_t>(GetFreeHeap(), MemoryUsed);
    }
}