enum class Types : uint8_t
{
    Undefined = 0,
    Folder, // 0 Extra bytes
    Shape2D,
    Byte, // uint_8
    Bool, // 1 byte
    Type,
    Function,
    Flags,
    Status,
    Board,
    Port,
    PortDriver,
    Fan,
    LEDStrip,
    LEDSegment,
    Texture1D,
    Display,
    Geometry2D,
    GeometryOperation,
    Texture2D,
    AnimationFloat,
    AnimationVector,
    AnimationCoord,
    AnimationColour,
    Operation, // Enum/value, not object anymore
    Program,   // List of Operations... execute and wait options (Sequence, All and wait, All no wait)
    Integer,   // int_32
    Time,      // uint_32
    Number,    // float_32
    ID,        // uint_32
    Colour,    // 4 byte RGBA
    PortAttach,
    Vector2D, // 8 byte (2xFloat)
    Coord2D,  // 16 byte (2xVector)
    Text,     // String, variable length
    IDList,   // List, variable length
};

int8_t GetValueSize(Types Type)
{
    if (Type < Types::Byte)
        return 0;
    else if (Type < Types::Integer)
        return sizeof(uint8_t);
    else if (Type < Types::Vector2D)
        return sizeof(int32_t);
    else if (Type == Types::Vector2D)
        return sizeof(float) * 2;
    else if (Type == Types::Coord2D)
        return sizeof(float) * 4;
    else
        return -1; // dynamic, check first byte
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
    SetFlags, //Swapped
    ReadModules,
    SetModules,
    WriteValue,
    ReadValue,
    ReadDatabase, // Debug, Serial only now
    ReadFile,
    RunFile,
    Refresh,
    
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
    ReportError(Status::InvalidType, "GetType Undefined, Size:" + String(sizeof(C)));
    return Types::Undefined;
};
template <>
Types GetType<Folder>() { return Types::Folder; };
template <>
Types GetType<u_int8_t>() { return Types::Byte; };
template <>
Types GetType<bool>() { return Types::Bool; };
template <>
Types GetType<Types>() { return Types::Type; };
template <>
Types GetType<Functions>() { return Types::Function; };
template <>
Types GetType<FlagClass>() { return Types::Flags; };
template <>
Types GetType<Status>() { return Types::Status; };
template <>
Types GetType<Boards>() { return Types::Board; };
template <>
Types GetType<Ports>() { return Types::Port; };
template <>
Types GetType<Drivers>() { return Types::PortDriver; };
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
Types GetType<FloatAnimations>() { return Types::AnimationFloat; };
// AnimVector,Colour
template <>
Types GetType<CoordAnimations>() { return Types::AnimationCoord; };
template <>
Types GetType<Operations>() { return Types::Operation; };
template <>
Types GetType<ProgramTypes>() { return Types::Program; };
template <>
Types GetType<int32_t>() { return Types::Integer; };
template <>
Types GetType<uint32_t>() { return Types::Time; };
template <>
Types GetType<float>() { return Types::Number; };
template <>
Types GetType<IDClass>() { return Types::ID; };
template <>
Types GetType<ColourClass>() { return Types::Colour; };
// Port Attach
template <>
Types GetType<Vector2D>() { return Types::Vector2D; };
template <>
Types GetType<Coord2D>() { return Types::Coord2D; };
template <>
Types GetType<String>() { return Types::Text; };
template <>
Types GetType<IDList>() { return Types::IDList; };
