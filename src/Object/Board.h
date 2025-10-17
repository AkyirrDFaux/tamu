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
    Folder Devices = Folder(true, RandomID, Flags::Auto);

    BoardClass(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup();
    void UpdateLoopTime();
};

BoardClass::BoardClass(bool New, IDClass ID, FlagClass Flags) : BaseClass(ID, Flags) // Board always loads first, it takes ID 1
{
    Type = Types::Board;
    Name = "Board";

    Values.Add(Boards::Undefined);
    //Values.Add(String("Unnamed")); //STRING BROKEN FOR NOW
    Values.Add((uint32_t)0, BootTime);
    Values.Add((uint32_t)0, AvgTime);
    Values.Add((uint32_t)0, MaxTime);

    AddModule(&Devices);
    Devices.Name = "Devices";
};

void BoardClass::Setup() // Load Presets
{
    for (int32_t Index = Devices.Modules.FirstValid(Types::Port); Index < Devices.Modules.Length; Devices.Modules.Iterate(&Index, Types::Port))
        delete Devices.Modules[Index];
    Devices.Modules.RemoveAll();

    if (*Values.At<Boards>(BoardType) == Boards::Tamu_v1_0)
    {
        Devices.AddModule(new PortClass(15, Ports::GPIO), 0); // J1
        Devices.AddModule(new PortClass(13, Ports::GPIO), 1); // J2
        Devices.AddModule(new PortClass(14, Ports::GPIO), 2); // J5
        Devices.AddModule(new PortClass(2, Ports::GPIO), 3);  // J6
        Devices.AddModule(new PortClass(12, Ports::TOut), 4); // J3
    }
    if (*Values.At<Boards>(BoardType) == Boards::Tamu_v2_0)
    {
        Devices.AddModule(new PortClass(0, Ports::GPIO), 0);  // P1
        Devices.AddModule(new PortClass(1, Ports::GPIO), 1);  // P2
        Devices.AddModule(new PortClass(9, Ports::GPIO), 2);  // P3 - S
        Devices.AddModule(new PortClass(10, Ports::TOut), 3); // P4 - F !!! Notation of switch is inverted

        Devices.AddModule(new PortClass(6, Ports::TOut), 4); // P5 - F
        Devices.AddModule(new PortClass(8, Ports::GPIO), 5); // P6 - S
        Devices.AddModule(new PortClass(7, Ports::GPIO), 6); // P7
        Devices.AddModule(new PortClass(3, Ports::GPIO), 7); // P8

        Devices.AddModule(new PortClass(4, Ports::SDA), 8);
        Devices.AddModule(new PortClass(5, Ports::SCL), 9);
        Devices.AddModule(new GyrAccClass(GyrAccs::LSM6DS3TRC), 10);
        Devices.Modules[10]->AddModule(Devices.Modules[8], 0);
        Devices.Modules[10]->AddModule(Devices.Modules[9], 1);
        Devices.Modules[10]->Setup();

        Devices.AddModule(new PortClass(LED_NOTIFICATION_PIN, Ports::GPIO), 11);
        Devices.AddModule(new InputClass(Inputs::ButtonWithLED), 12);
        Devices.Modules[12]->AddModule(Devices.Modules[11], 0);
        // Devices.Modules[12]->Setup();
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