enum class Types : uint8_t
{
    Undefined = 0,
    Bool,       // 1 byte
    Byte,       // uint_8
    PortNumber, // int_8
    Type,       // 1 byte enums
    ObjectType,
    ObjectInfo,
    Status,
    Board,
    Sensor,
    PortDriver,
    I2CDevice,
    Input,
    LEDStrip,
    Texture1D,
    Display,
    Geometry2D,
    GeometryOperation,
    Texture2D,
    Operation,
    Program,
    Output,
    Function,
    Group,
    Integer,   // int_32
    Number,    // float_32
    PortType,  // 4 byte flag
    Pin,       // 2 byte - start packed
    Colour,    // 4 byte RGBA
    Vector2D,  // 8 byte (2xNumber)
    Vector3D,  // 12 byte (3xNumber)
    Coord2D,   // 16 byte (2xVector2D)
    Coord3D,   // 24 byte (2xVector3D)
    Text,      // String - variable length
    Reference, // uint8 array
    Message    // raw data array
};

bool IsPacked(const Types Type) // Contains subparts
{
    if (Type >= Types::Pin)
        return true;
    else
        return false;
}
// Returns true if the type is a POD scalar
inline bool IsScalar(Types T)
{
    return (T == Types::Number || T == Types::Integer ||
            T == Types::Byte || T == Types::Bool);
}

enum Flags : uint8_t
{
    None = 0,
    Auto = 0b00000001, // Automatically created (built-in)
    Dirty = 0b00000010, //Made a non-read only change
    Undefined2 = 0b00000100,
    Undefined3 = 0b00001000,
    RunLoop = 0b00010000,
    RunOnce = 0b00100000,      // Run once manually until finished, will reset the flag automatically
    RunOnStartup = 0b01000000, // Run automatically once after board finished loading
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
    AutoObject,
    OutOfFlash,
    NotInFlash,
    FlashWritten
};

void ReportError(Status ErrorCode);
void ReportError(Status ErrorCode, IDClass ID);

enum class ObjectTypes : uint8_t
{
    Undefined = 0,
    Board,
    LEDStrip,
    Display,
    Input,
    Sensor,
    Output,
    Program,
    I2C,
    UART,
    SPI,
    OLED
};

enum class Functions : uint8_t
{
    None = 0,
    Report,
    CreateObject, // Create new
    DeleteObject, // Delete object
    LoadObject,   // Create from ValueTree
    SaveObject,   // Save to file
    SaveAll,
    ReadObject, // Send to app
    Refresh,
    ReadValue,
    WriteValue,
    ReadName,
    WriteName,
    ReadInfo,
    SetInfo,
    ReadFile
};

void Run(const FlexArray &Input); // Switcher
void CreateObject(const FlexArray &Input);
void DeleteObject(const FlexArray &Input);
void LoadObject(const FlexArray &Input);
void SaveObject(const FlexArray &Input);
void SaveAll(const FlexArray &Input);
void ReadObject(const FlexArray &Input);
void Refresh(const FlexArray &Input);
void ReadValue(const FlexArray &Input);
void WriteValue(const FlexArray &Input);
void ReadName(const FlexArray &Input);
void WriteName(const FlexArray &Input);
void ReadInfo(const FlexArray &Input);
void SetInfo(const FlexArray &Input);
// void ReadFile(ValueTree &Input);

template <class C>
Types GetType()
{
    ReportError(Status::InvalidType);
    return Types::Undefined;
};

// Data
template <>
Types GetType<bool>() { return Types::Bool; };
template <>
Types GetType<uint8_t>() { return Types::Byte; };
template <>
Types GetType<int32_t>() { return Types::Integer; };
template <>
#if USE_FIXED_POINT == 1
Types GetType<Number>()
{
    return Types::Number;
};
template <>
#else
Types GetType<float>()
{
    return Types::Number;
};
template <>
#endif
Types GetType<Reference>()
{
    return Types::Reference;
};
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

// Enums
template <>
Types GetType<ObjectTypes>() { return Types::ObjectType; };
template <>
Types GetType<Functions>() { return Types::Function; };
template <>
Types GetType<ObjectInfo>() { return Types::ObjectInfo; };
template <>
Types GetType<Status>() { return Types::Status; };
template <>
Types GetType<Boards>() { return Types::Board; };
template <>
Types GetType<SensorTypes>() { return Types::Sensor; };
template <>
Types GetType<Drivers>() { return Types::PortDriver; };
template <>
Types GetType<I2CDevices>() { return Types::I2CDevice; };
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

template <>
Types GetType<Pin>() { return Types::Pin; };
template <>
Types GetType<PortNumber>() { return Types::PortNumber; };
template <>
Types GetType<Outputs>() { return Types::Output; };