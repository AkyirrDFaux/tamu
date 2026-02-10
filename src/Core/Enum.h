enum class Types : uint8_t
{
    Undefined = 0,
    Bool, // 1 byte
    Byte, // uint_8
    Integer, // int_32
    Number,  // float_32
    Time,    // uint_32
    ID,      // uint_32
    Colour,  // 4 byte RGBA
    Vector2D, // 8 byte (2xNumber)
    Vector3D, // 12 byte (3xNumber)
    Coord2D,  // 16 byte (2xVector2D)
    Coord3D,  // 24 byte (2xVector3D)
    Text,     // String, variable length
    IDList,   // List, variable length
    ObjectType = 32, //1 byte enums
    Function,
    Flags,
    Status,
    Board,
    Sensor,
    PortDriver,
    AccGyr,
    Input,
    LEDStrip,
    Texture1D,
    Display,
    Geometry2D,
    GeometryOperation,
    Texture2D,
    Operation, 
    Program,
    PortType = 64, //4 byte enums
};

int8_t GetDataSize(const Types Type)
{
    if (Type == Types::Undefined)
        return 0;
    else if (Type <= Types::Byte || (Type >= Types::ObjectType && Type < Types::PortType)) 
        return 1;
    else if (Type <= Types::Colour || Type >= Types::PortType)
        return 4;
    else if (Type == Types::Vector2D)
        return 8;
    else if (Type == Types::Vector3D)
        return 12;
    else if (Type == Types::Coord2D)
        return 16;
    else if (Type == Types::Coord3D)
        return 24;
    else
        return -1; // dynamic, check first byte
};

enum Flags : uint8_t
{
    None = 0,
    Auto = 0b00000001,         // No name change, cannot remove (ex. Board, Port, built-in devices)
    System = 0b00000010,       // No value editing, no saving (ex. Port, some built-in devices)
    Undefined2 = 0b00000100,      
    RunLoop = 0b00001000,      // Allow automatic run forever
    RunOnce = 0b00010000,      // Run once manually until finished, will reset the flag automatically
    RunOnStartup = 0b00100000, // Run automatically once after board finished loading
    Favourite = 0b01000000,    // Show when filtered
    Inactive = 0b10000000      // Ignore object
};

enum class Status : uint8_t
{
    OK = 0,
    InvalidID,
    InvalidType,
    InvalidFunction,
    InvalidValue,
    MissingModule,
    FileError,
    PortError,
    NoValue,
    AutoObject
};

enum class ObjectTypes : uint8_t
{
    Undefined = 0,
    Shape2D,
    Board,
    Port,
    Fan,
    LEDStrip,
    LEDSegment,
    Texture1D,
    Display,
    Geometry2D,
    Texture2D,
    AccGyr, 
    Servo, 
    Input, 
    Operation, 
    Program,
    I2C,
    UART,
    SPI,
    Sensor
};

enum class Functions : uint8_t
{
    None = 0,
    CreateObject, // Create new
    DeleteObject, // Delete object
    SaveObject,   // Save to file
    SaveAll,
    LoadObject, // Create from ByteArray
    ReadObject, // Send to app
    ReadType,
    ReadName,
    WriteName,
    SetFlags, // Swapped
    ReadModules,
    SetModules,
    WriteValue,
    ReadValue,
    ReadDatabase, // Debug, Serial only now
    ReadFile,
    RunFile,
    Refresh
};

void Run(ByteArray &Input);
void ReadDatabase(ByteArray &Input);
void ReadValue(ByteArray &Input);
void WriteValue(ByteArray &Input);
void CreateObject(ByteArray &Input);
void DeleteObject(ByteArray &Input);
void SaveObject(ByteArray &Input);
void WriteName(ByteArray &Input);
void ReadName(ByteArray &Input);
void ReadFile(ByteArray &Input);
void SaveAll(ByteArray &Input);
void ReadObject(ByteArray &Input);
void LoadObject(ByteArray &Input);
void RunFile(ByteArray &Input);
void Refresh(ByteArray &Input);
void SetModules(ByteArray &Input);
void SetFlags(ByteArray &Input);

template <class C>
Types GetType()
{
    ReportError(Status::InvalidType);
    return Types::Undefined;
};

//Data
template <>
Types GetType<bool>() { return Types::Bool; };
template <>
Types GetType<uint8_t>() { return Types::Byte; };
template <>
Types GetType<int32_t>() { return Types::Integer; };
template <>
#if USE_FIXED_POINT == 1
Types GetType<Number>() { return Types::Number; };
template <>
#else
Types GetType<float>() { return Types::Number; };
template <>
#endif
Types GetType<uint32_t>() { return Types::Time; };
template <>
Types GetType<IDClass>() { return Types::ID; };
template <>
Types GetType<ColourClass>() { return Types::Colour; };
template <>
Types GetType<Vector2D>() { return Types::Vector2D; };
template <>
Types GetType<Vector3D>() { return Types::Vector3D; };
template <>
Types GetType<Coord2D>() { return Types::Coord2D; };
template <>
Types GetType<Text>() { return Types::Text; };
template <>
Types GetType<IDList>() { return Types::IDList; };

//Enums
template <>
Types GetType<ObjectTypes>() { return Types::ObjectType; };
template <>
Types GetType<Functions>() { return Types::Function; };
template <>
Types GetType<FlagClass>() { return Types::Flags; };
template <>
Types GetType<Status>() { return Types::Status; };
template <>
Types GetType<Boards>() { return Types::Board; };
template <>
Types GetType<SensorTypes>() { return Types::Sensor; };
template <>
Types GetType<Drivers>() { return Types::PortDriver; };
template <>
Types GetType<GyrAccs>() { return Types::AccGyr; };
template <>
Types GetType<Inputs>() { return Types::Input; };
template <>
Types GetType<LEDStrips>() { return Types::LEDStrip; };
template <>
Types GetType<Textures1D>() { return Types::Texture1D; };
template <>
Types GetType<Displays>() { return Types::Display; };
template <>
Types GetType<Geometries>() { return Types::Geometry2D; };
template <>
Types GetType<GeometryOperation>() { return Types::GeometryOperation; };
template <>
Types GetType<Textures2D>() { return Types::Texture2D; };
template <>
Types GetType<Operations>() { return Types::Operation; };
template <>
Types GetType<ProgramTypes>() { return Types::Program; };
template <>
Types GetType<PortTypeClass>() { return Types::PortType; };
