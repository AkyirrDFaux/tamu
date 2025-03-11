class BoardClass : public Variable<Boards>
{
public:
    Folder Port = Folder(true, RandomID, Flags::Auto);
    Variable<String> BTName = Variable<String>("Unnamed");
    Variable<uint32_t> BootTime = Variable<uint32_t>(0, RandomID, Flags::Auto);
    Variable<uint32_t> AvgLoopTime = Variable<uint32_t>(0, RandomID, Flags::Auto);
    Variable<uint32_t> MaxLoopTime = Variable<uint32_t>(0, RandomID, Flags::Auto);

    BoardClass(bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);
    void Setup();
    void UpdateLoopTime();
};

BoardClass::BoardClass(bool New, IDClass ID, FlagClass Flags) : Variable(Boards::Undefined ,ID, Flags) // Board always loads first, it takes ID 1
{
    Type = Types::Board;
    Name = "Board";
    AddModule(&Port);
    Port.Name = "Ports";
    AddModule(&BootTime);
    BootTime.Name = "Boot Time";
    AddModule(&AvgLoopTime);
    AvgLoopTime.Name = "Avg. Loop Time";
    AddModule(&MaxLoopTime);
    MaxLoopTime.Name = "Max. Loop Time";
    AddModule(&BTName);
    BTName.Name = "Bluetooth Name";
};

void BoardClass::Setup() // Load Presets
{
    for (int32_t Index = Port.Modules.FirstValid(Types::Port); Index < Port.Modules.Length; Port.Modules.Iterate(&Index, Types::Port))
        delete Port.Modules[Index];
    Port.Modules.RemoveAll();

    if (*ValueAs<Boards>() == Boards::Tamu_v1_0)
    {
        Port.AddModule(new PortClass(15, Ports::GPIO), 0); // J1
        Port.AddModule(new PortClass(13, Ports::GPIO), 1); // J2
        Port.AddModule(new PortClass(14, Ports::GPIO), 2); // J5
        Port.AddModule(new PortClass(2, Ports::GPIO), 3);  // J6
        Port.AddModule(new PortClass(12, Ports::TOut), 4); // J3
    }
    if (*ValueAs<Boards>() == Boards::Tamu_v2_0)
    {
        Port.AddModule(new PortClass(0, Ports::GPIO), 0); // P1
        Port.AddModule(new PortClass(1, Ports::GPIO), 1); // P2
        Port.AddModule(new PortClass(9, Ports::GPIO), 2); // P3 - S
        Port.AddModule(new PortClass(10, Ports::TOut), 3); // P4 - F !!! Notation of switch is inverted

        Port.AddModule(new PortClass(6, Ports::TOut), 4);  // P5 - F
        Port.AddModule(new PortClass(8, Ports::GPIO), 5);  // P6 - S
        Port.AddModule(new PortClass(7, Ports::GPIO), 6);  // P7
        Port.AddModule(new PortClass(3, Ports::GPIO), 7); // P8
    }
};

void BoardClass::UpdateLoopTime()
{
    AvgLoopTime = (DeltaTime + AvgLoopTime * 15) / 16;
    if (DeltaTime > MaxLoopTime)
        MaxLoopTime = DeltaTime;
    else if (millis() % 20000 < 20)
        MaxLoopTime = (uint32_t)AvgLoopTime; //It will copy the object otherwise
}