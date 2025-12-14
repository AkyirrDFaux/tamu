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
    Port,
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
    Program
};

int8_t GetDataSize(Types Type)
{
    if (Type == Types::Undefined)
        return 0;
    else if (Type <= Types::Byte || Type >= Types::ObjectType)
        return 1;
    else if (Type <= Types::Colour)
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

template <class C>
Types GetType()
{
    ReportError(Status::InvalidType, "GetType Undefined, Size:" + String(sizeof(C)));
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
Types GetType<float>() { return Types::Number; };
template <>
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
Types GetType<String>() { return Types::Text; };
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
Types GetType<Ports>() { return Types::Port; };
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
Types GetType<Types>() { return Types::Program; };


