class DisplayClass : public BaseClass
{
public:
    uint8_t *Layout = nullptr; // Layout mapping for shaped display
    Reference CurrentLink = Reference(0,0,0);  // Backup for disconnecting
    uint8_t *LEDs = nullptr;   // Memory buffer

    DisplayClass(Reference ID, ObjectInfo Info = {Flags::None, 1});
    ~DisplayClass();

    void Setup(Path Index);
    bool Run();

    bool Connect(BaseClass *Object = nullptr, int32_t Index = -1);
    bool Disconnect(BaseClass *Object = nullptr);

    static void SetupBridge(BaseClass *Base, Path Index) { static_cast<DisplayClass *>(Base)->Setup(Index); }
    static bool RunBridge(BaseClass *Base) { return static_cast<DisplayClass *>(Base)->Run(); }
    static constexpr VTable Table = {
        .Setup = DisplayClass::SetupBridge,
        .Run = DisplayClass::RunBridge};
};

constexpr VTable DisplayClass::Table;

DisplayClass::DisplayClass(Reference ID, ObjectInfo Info) : BaseClass(&Table, ID, Info)
{
    BaseClass::Type = ObjectTypes::Display;
    Name = "Display";

    Values.Set(Displays::Undefined, {0});
    Values.Set<Reference>(Reference(0,0,0), {0, 0}); // Port Number
    Values.Set<int32_t>(0, {0, 1});       // Length
    Values.Set(Vector2D(0, 0), {0, 2});   // Size
    Values.Set<Number>(1, {0, 3});        // Ratio
    Values.Set<uint8_t>(20, {0, 4});      // Brightness
    Values.Set(Coord2D(0, 0, 0), {0, 5}); // Offset
    Values.Set(false, {0, 6});            // Mirroring
    Values.Set(false, {0, 7});            // Wraping X
    Values.Set(false, {0, 8});            // Wraping Y
    // Values.Set({1}); //Render group
};

DisplayClass::~DisplayClass()
{
    Disconnect();
};

bool DisplayClass::Connect(BaseClass *Object, int32_t Index)
{
    ESP_LOGI("Display","- Connect(Idx: %d)\n", Index);
    if (Object == nullptr) Object = this;

    Getter<Reference> Link = Values.Get<Reference>({0, 0});
    if (!Link.Success || Link.Value == Reference(0,0,0)) return false;

    BaseClass *Target = Objects.At(Link.Value);
    if (!Target) return false;

    this->CurrentLink = Link.Value;

    if (Target->Type == ObjectTypes::Board) {
        // We are hitting the hardware anchor; Port is now mandatory.
        if (Link.Value.Location.Length < 2) return false;
        
        uint8_t Port = Link.Value.Location.Indexing[1];
        return static_cast<BoardClass *>(Target)->Connect(Object, Port, Index + 1);
    } 
    else {
        // We are hitting another peripheral; Port is irrelevant to this hop.
        return static_cast<DisplayClass *>(Target)->Connect(Object, Index + 1);
    }
}

bool DisplayClass::Disconnect(BaseClass *Object)
{
    if (Object == nullptr) Object = this;
    if (CurrentLink == Reference(0,0,0)) return false;

    BaseClass *Target = Objects.At(CurrentLink);
    if (!Target) return false;

    bool Success = false;

    if (Target->Type == ObjectTypes::Board) {
        // Port is required to tell the Board which folder to clean.
        if (CurrentLink.Location.Length < 2) return false;

        uint8_t Port = CurrentLink.Location.Indexing[1];
        Success = static_cast<BoardClass *>(Target)->Disconnect(Object, Port);
    } 
    else {
        // Handshake to the next device in the chain.
        Success = static_cast<DisplayClass *>(Target)->Disconnect(Object);
    }

    if (Object == this) {
        this->LEDs = nullptr;
        this->CurrentLink = Reference(0,0,0);
    }
    
    return Success;
}

void DisplayClass::Setup(Path Index)
{
    ESP_LOGI("Display", "- Setup Triggered");
    bool HardwareChanged = false;

    // All relevant changes start with 0 in this object's local tree
    if (Index.Length > 0 && Index.Indexing[0] == 0)
    {
        // 1. If Index is just {0}, the Display Type changed
        if (Index.Length == 1)
        {
            Displays Type = Values.Get<Displays>({0});
            ESP_LOGI("Display", "Type changed to %d", (int)Type);

            switch (Type)
            {
                case Displays::GenericLEDMatrix:
                    Values.Set<int32_t>(256, {0, 1});     // Update Length
                    Values.Set(Vector2D(16, 16), {0, 2}); // Update Size
                    HardwareChanged = true;
                    break;
                case Displays::Vysi_v1_0:
                    Values.Set<int32_t>(86, {0, 1});
                    Values.Set(Vector2D(11, 10), {0, 2});
                    HardwareChanged = true;
                    break;
                default:
                    break;
            }
        }
        // 2. If Index is {0, 0}, the Reference (Port Link) changed
        else if (Index.Length > 1 && Index.Indexing[1] == 0)
        {
            ESP_LOGI("Display", "Link/Reference changed");
            HardwareChanged = true;
        }
        // 3. If Index is {0, 1}, the Length changed
        else if (Index.Length > 1 && Index.Indexing[1] == 1)
        {
            ESP_LOGI("Display", "Chain Length changed");
            HardwareChanged = true;
        }
    }

    // Perform Re-connection if any critical parameters changed
    if (HardwareChanged)
    {
        ESP_LOGI("Display", "Executing Reconnect Sequence");
        Disconnect(); 
        Connect();    
    }
}

bool DisplayClass::Run() {
    /*if (Values.Type(Length) != Types::Integer || Values.Type(Size) != Types::Vector2D || Values.Type(Ratio) != Types::Number ||
        Values.Type(Brightness) != Types::Byte || Values.Type(Offset) != Types::Coord2D || Values.Type(Mirrored) != Types::Bool)
    {
        ReportError(Status::MissingModule, ID);
        return true;
    }

    if (LEDs.LEDs == nullptr)
    {
        ReportError(Status::PortError, ID);
        return true;
    }

    uint32_t Length = ValueGet<int32_t>(Value::Length);
    Vector2D Size = ValueGet<Vector2D>(Value::Size);
    Number Ratio = ValueGet<Number>(Value::Ratio);
    uint8_t Brightness = ValueGet<uint8_t>(Value::Brightness);
    Coord2D Offset = ValueGet<Coord2D>(Value::Offset);
    bool Mirrored = ValueGet<bool>(Value::Mirrored);

    ColourClass Buffer[Length];
    Coord2D Transform = Coord2D(Size * 0.5 - Vector2D(0.5, 0.5), Vector2D(0)).Join(Offset);
    // Iterate over layers
    for (uint32_t Index = Modules.FirstValid(ObjectTypes::Shape2D); Index < Modules.Length; Modules.Iterate(&Index, ObjectTypes::Shape2D))
        Modules[Index]->As<Shape2DClass>()->Render(Length, Size, Ratio, Transform, Mirrored, Layout, Buffer);

    // Final output
    for (uint32_t Index = 0; Index < Length; Index++)
    {
        Buffer[Index].ToDisplay(Brightness);
        LEDs.Write(Index, Buffer[Index]);
    }
    */
    return true;
};