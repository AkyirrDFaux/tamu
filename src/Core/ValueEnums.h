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
    TOut,
    SDA,
    SCL
};

enum class Drivers : uint8_t
{
    None,
    LED,
    FanPWM,
    I2C,
    I2CClock,
    Servo,
    Input
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
    GenericRGB,
    GenericRGBW
};

enum class GyrAccs :uint8_t
{
    Undefined,
    LSM6DS3TRC
};

enum class Inputs :uint8_t
{
    Undefined,
    Button,
    ButtonWithLED,
    Analog
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
    Extract,
    Combine,
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
    MoveTo, //Animations
    Delay, //Delay for specified amount of time (parameter), holds start time
    AddDelay, //Create delayed time (for other operations)
    IfSwitch,//Program control (branch)
    While,
    SetFlags,
    ResetFlags
};