struct Pin
{
    uint8_t Number;
    char Port = 0;
};

struct PortNumber
{
    int8_t Value;
    PortNumber(int8_t Input) : Value(Input) {};
    PortNumber() : Value(-1) {};
    operator uint8_t() const { return (uint8_t)Value; }
    bool IsValid() const { return Value >= 0; }
};

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
    Input,
    Analog,
    Output,
    LED,
    I2C_SDA,
    I2C_SCL,
    UART_TX,
    UART_RX,
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
    GenericLEDMatrix,
    GenericLEDMatrixWeave,
    Vysi_v1_0 = 10
};

enum class LEDStrips : uint8_t
{
    Undefined,
    GenericRGB,
    GenericRGBW
};

enum class I2CDevices : uint8_t
{
    Undefined,
    LSM6DS3TRC,
    BMI160
};

enum class Inputs : uint8_t
{
    Undefined,
    Button,
    ButtonInverted,
    ButtonWithLED,
    ButtonWithLEDInverted
};

enum class SensorTypes : uint8_t
{
    Undefined,
    AnalogVoltage,
    TempNTC10K,
    Light10K
};

enum class Outputs : uint8_t
{
    Undefined,
    PWM,
    Servo
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
    Set,
    Delete,
    ToBool,
    ToByte,
    ToInt,
    ToNumber,
    ToVector2D,
    ToVector3D,
    ToCoord2D,
    ToColour,
    ToColourHSV,
    Extract,
    IsEqual, // Comparision
    IsGreater,
    IsSmaller,
    Add, // Math
    Subtract,
    Multiply,
    Divide,
    Power,
    Absolute,
    Min,    
    Max,    
    Modulo, 
    Random,   // Randomize (Min, Max)
    Sine,   // Waveform
    Square,
    Sawtooth,
    Magnitude, // Vector math
    Angle,
    Normalize,
    DotProduct,
    CrossProduct,
    Clamp, // Mapping
    Deadzone,
    LinInterpolation,
    And, // Logic
    Or,
    Not,
    Edge,
    SetReset,
    Delay,    // Delay for specified amount of time (parameter), holds start time
    IfSwitch, // Program control (branch)
    SetRunOnce,
    SetRunLoop,
    SetReference,
    Save,
};