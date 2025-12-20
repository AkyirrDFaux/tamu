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
    Point,
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
    ResetFlags,
    Sine
};