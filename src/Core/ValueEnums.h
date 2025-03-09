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

enum class Boards : uint8_t
{
    Undefined = 0,
    Tamu_v1_0,
    Tamu_v2_0
};

enum class Ports : uint8_t
{
    None = 0,
    GPIO,
    TOut
};

enum class Drivers : uint8_t
{
    None,
    LED,
    FanPWM
};

enum class Geometries : uint8_t
{
    None = 0,
    Box,
    Elipse,
    Triangle,
    Polygon,
    Star,
    DoubleParabola,
    Fill,
    HalfFill
};

enum class Textures1D : uint8_t
{
    None = 0,
    Full,
    Blend,
    Noise
};

enum class Textures2D : uint8_t
{
    None = 0,
    Full,
    BlendLinear,
    BlendCircular,
    Noise
};

enum class GeometryOperation : uint8_t
{
    Add,
    Cut,
    Intersect,
    XOR
};

enum class Displays : uint8_t
{
    Undefined,
    Vysi_v1_0
};

enum class LEDStrips : uint8_t
{
    Undefined,
    Generic
};

enum class FloatAnimations : uint8_t
{
    None,
    MoveTo,
    MoveBetween
};

enum class VectorAnimations : uint8_t
{
    None,
    MoveTo,
    MoveBetween
};

enum class CoordAnimations : uint8_t
{
    None,
    MoveTo
};

enum class ColourAnimations : uint8_t
{
    None,
    TimeBlend,
    BlendLoop,
    BlendRGB
};

enum class ProgramTypes : uint8_t
{
    None,
    Sequence,
    All
};

enum class Operations : uint8_t
{
    None,
    Equal,
    IsEqual,//Comparision
    IsGreater,
    IsSmaller,
    Add,//Math
    Subtract,
    Multiply,
    Divide,
    Power,
    Absolute,
    Rotate,
    RandomBetween,//Randomize
    AddDelay, //Create delayed time
    IfSwitch,//Program control (branch)
    While,
    SetActivity
};