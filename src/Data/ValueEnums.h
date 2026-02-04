enum class Boards : uint8_t
{
    Undefined = 0,
    Tamu_v1_0,
    Tamu_v2_0,
    Valu_v2_0 = 12
};
namespace Ports
{
    enum Ports : uint32_t
    {
        None = 0,
        GPIO = (1 << 0),
        ADC = (1 << 1),
        PWM = (1 << 2),
        TOut = (1 << 3),
        Internal = (1 << 4),
        I2C_SDA = (1 << 8),
        I2C_SCL = (1 << 9),
        UART_TX = (1 << 12),
        UART_RX = (1 << 13),
        SPI_MOSI = (1 << 16),
        SPI_MISO = (1 << 17),
        SPI_CLK = (1 << 18),
        SPI_CS = (1 << 19),
        SPI_DRST = (1 << 20),
        SPI_DDC = (1 << 21),
    };
}

enum class Drivers : uint8_t
{
    None,
    LED,
    FanPWM,
    I2C,  // Any function of bus
    UART, // Any function of bus
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

enum class GyrAccs : uint8_t
{
    Undefined,
    LSM6DS3TRC
};

enum class Inputs : uint8_t
{
    Undefined,
    Button,
    ButtonWithLED,
};

enum class SensorTypes : uint8_t
{
    Undefined,
    AnalogVoltage,
    TempNTC10K,
    Light10K
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
    IsEqual, // Comparision
    IsGreater,
    IsSmaller,
    Add, // Math
    Subtract,
    Multiply,
    Divide,
    Power,
    Absolute,
    Rotate,
    RandomBetween, // Randomize
    MoveTo,        // Animations
    Delay,         // Delay for specified amount of time (parameter), holds start time
    AddDelay,      // Create delayed time (for other operations)
    IfSwitch,      // Program control (branch)
    While,
    SetFlags,
    ResetFlags,
    Sine
};