class BoardClass : public BaseClass
{
public:
    enum Value
    {
        BoardType,
        BTName,
        BootTime,
        AvgTime,
        MaxTime
    };

    BoardClass(IDClass ID = (1 << 8), FlagClass Flags = Flags::None);
    void Setup(int32_t Index = -1);
    void UpdateLoopTime();
};

BoardClass::BoardClass(IDClass ID, FlagClass Flags) : BaseClass(ID, Flags) // Board always loads first, it takes ID 1
{
    Type = ObjectTypes::Board;
    Name = "Board";

    Values.Add(Boards::Undefined);
    Values.Add(String("Unnamed"));
    Values.Add((uint32_t)0, BootTime);
    Values.Add((uint32_t)0, AvgTime);
    Values.Add((uint32_t)0, MaxTime);
};

void BoardClass::Setup(int32_t Index) // Load Presets
{
    if (Index == 0 && Values.TypeAt(0) == Types::Board && *Values.At<Boards>(0) == Boards::Undefined) //Double setup prevention
    {
#if BOARD == 1
        *Values.At<Boards>(0) = Boards::Tamu_v1_0;
        AddModule(new PortClass(15, Ports::GPIO), 0); // J1
        AddModule(new PortClass(13, Ports::GPIO), 1); // J2
        AddModule(new PortClass(14, Ports::GPIO), 2); // J5
        AddModule(new PortClass(2, Ports::GPIO), 3);  // J6
        AddModule(new PortClass(12, Ports::TOut), 4); // J3
#elif BOARD == 2
        *Values.At<Boards>(0) = Boards::Tamu_v2_0;
        AddModule(new PortClass(0, Ports::GPIO), 0);  // P1
        AddModule(new PortClass(1, Ports::GPIO), 1);  // P2
        AddModule(new PortClass(9, Ports::GPIO), 2);  // P3 - S
        AddModule(new PortClass(10, Ports::TOut), 3); // P4 - F !!! Notation of switch is inverted

        AddModule(new PortClass(6, Ports::TOut), 4); // P5 - F
        AddModule(new PortClass(8, Ports::GPIO), 5); // P6 - S
        AddModule(new PortClass(7, Ports::GPIO), 6); // P7
        AddModule(new PortClass(3, Ports::GPIO), 7); // P8

        AddModule(new PortClass(4, Ports::SDA), 8);
        AddModule(new PortClass(5, Ports::SCL), 9);
        AddModule(new PortClass(LED_NOTIFICATION_PIN, Ports::GPIO), 10);

        AddModule(new GyrAccClass(), 11);
        Modules[11]->AddModule(Modules[8], 0);
        Modules[11]->AddModule(Modules[9], 1);
        Modules[11]->ValueSet<GyrAccs>(GyrAccs::LSM6DS3TRC, 0);

        AddModule(new InputClass(), 12);
        Modules[12]->AddModule(Modules[10], 0);
        Modules[12]->ValueSet<Inputs>(Inputs::ButtonWithLED, 0);
#endif
    }
};

void BoardClass::UpdateLoopTime()
{
    uint32_t *AvgLoopTime = Values.At<uint32_t>(AvgTime);
    uint32_t *MaxLoopTime = Values.At<uint32_t>(MaxTime);

    *AvgLoopTime = (DeltaTime + *AvgLoopTime * 15) / 16;
    if (DeltaTime > *MaxLoopTime)
        *MaxLoopTime = DeltaTime;
    else if (millis() % 20000 < 20)
        *MaxLoopTime = *AvgLoopTime; // It will copy the object otherwise
}