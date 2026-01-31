#if USE_FIXED_POINT == 1
#define DECIMAL 16

#define PI_F (PI * (1 << DECIMAL))
#define SIN_B ((4 << DECIMAL) / PI)
#define SIN_C ((4 << DECIMAL) / sq(PI))

class Number
{
public:
    int32_t Value = 0;

    Number() : Value(0) {}
    Number(int32_t NewValue) : Value(NewValue << DECIMAL) {}
    Number(uint32_t NewValue) : Value(int32_t(NewValue << DECIMAL)) {}
    Number(int NewValue) : Value(NewValue << DECIMAL) {}
    Number(float NewValue) : Value(int32_t(round(NewValue * (1 << DECIMAL)))) {}
    Number(double NewValue) : Value(int32_t(round(NewValue * (1 << DECIMAL)))) {}

    // Conversion back to float
    inline operator float() const { return float(Value) / (1 << DECIMAL); }

    inline Number &operator+=(const Number &Other)
    {
        Value += Other.Value;
        return *this;
    }
    inline Number &operator-=(const Number &Other)
    {
        Value -= Other.Value;
        return *this;
    }
    inline Number operator+(const Number &Other) const { return Number(*this) += Other; }
    inline Number operator-(const Number &Other) const { return Number(*this) -= Other; }
    inline Number operator-() const
    {
        Number Result;
        Result.Value = -Value;
        return Result;
    }

    inline Number operator*(const Number &Other) const
    {
        Number Result;
        Result.Value = int32_t((int64_t(Value) * Other.Value) >> DECIMAL);
        return Result;
    }

    inline Number operator/(const Number &Other) const
    {
        Number Result;
        if (Other.Value == 0)
        {
            Result.Value = 0;
            return Result;
        }
        Result.Value = int32_t((int64_t(Value) << DECIMAL) / Other.Value);
        return Result;
    }

    template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
    inline Number operator+(T Other) const { return *this + Number(Other); }
    template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
    inline Number operator-(T Other) const { return *this - Number(Other); }
    template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
    inline Number operator*(T Other) const { return *this * Number(Other); }
    template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
    inline Number operator/(T Other) const { return *this / Number(Other); }

    inline bool operator==(const Number &Other) const { return Value == Other.Value; }
    inline bool operator!=(const Number &Other) const { return Value != Other.Value; }
    inline bool operator<(const Number &Other) const { return Value < Other.Value; }
    inline bool operator>(const Number &Other) const { return Value > Other.Value; }
    inline bool operator<=(const Number &Other) const { return Value <= Other.Value; }
    inline bool operator>=(const Number &Other) const { return Value >= Other.Value; }

    template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
    inline bool operator<(T Other) const { return *this < Number(Other); }
    template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
    inline bool operator>(T Other) const { return *this > Number(Other); }
};

template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
inline Number operator+(T A, const Number &B) { return Number(A) + B; }
template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
inline Number operator-(T A, const Number &B) { return Number(A) - B; }
template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
inline Number operator*(T A, const Number &B) { return Number(A) * B; }
template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
inline Number operator/(T A, const Number &B) { return Number(A) / B; }

inline Number abs(Number A)
{
    Number Result;
    Result.Value = (A.Value < 0) ? -A.Value : A.Value;
    return Result;
}

inline Number sqrt(Number A)
{
    // Digit by digit algorithm
    uint64_t Temp = (uint64_t)A.Value << 16;
    uint64_t Result = 0;
    uint64_t Bit = (uint64_t)1 << 62;
    Number Return;

    // Skip inital zeros
    while (Bit > Temp)
        Bit >>= 2;

    while (Bit != 0)
    {
        if (Temp >= Result + Bit)
        {
            Temp -= Result + Bit;
            Result = (Result >> 1) + Bit;
        }
        else
            Result >>= 1;
        Bit >>= 2;
    }
    Return.Value = (uint32_t)Result;
    return Return;
}

inline Number sin(Number X)
{
    int32_t x = X.Value;
    Number Result;

    x = x % int32_t(2 * PI_F);
    if (x > PI_F)
        x -= 2 * PI_F;
    else if (x < -PI_F)
        x += 2 * PI_F;

    int32_t P1 = ((int64_t)SIN_B * x) >> 16;
    int32_t P2 = ((int64_t)SIN_C * (((int64_t)x * ((x < 0) ? -x : x)) >> 16)) >> 16;

    Result.Value = P1 - P2;
    return Result;
};

inline Number cos(Number X)
{
    return sin(X + PI / 2);
};

inline Number atan2(Number Y, Number X)
{
    // This is not very accurate
    int32_t Sign = Y > 0 ? 1 : -1;
    if (X < 0)
        return Sign * (3 * PI / 4 - PI / 4 * ((X + abs(Y)) / (abs(Y) - X)));
    else
        return Sign * (PI / 4 - PI / 4 * ((X - abs(Y)) / (X + abs(Y))));
};

inline Number min(Number A, Number B) { return (A.Value < B.Value) ? A : B; }
inline Number max(Number A, Number B) { return (A.Value > B.Value) ? A : B; }

#else
typedef float Number;
#endif

inline Number LimitZeroToOne(Number Value)
{
    return min(max(Value, Number(0)), Number(1));
};

inline uint8_t LimitByte(int Number)
{
    return min(max(Number, 0), 255);
};

inline Number ByteToPercent(byte Value)
{
    return LimitZeroToOne(Number(Value) / 255);
};

inline uint8_t PercentToByte(Number Value)
{
    return LimitByte((int32_t)Value * 255);
};

inline uint8_t MultiplyBytePercentByte(byte Number, byte Percent)
{
    return (uint8_t)(((int)Number * (int)Percent) / 255);
};

Number TimeStep(uint32_t TargetTime)
{
    if (CurrentTime >= TargetTime)
        return 1;

    uint32_t RemainingTime = TargetTime - CurrentTime;

    if (RemainingTime == 0)
        return 0;

    Number Prediction = Number(DeltaTime) / Number(RemainingTime);

    return LimitZeroToOne(Prediction);
};

inline Number TimeMove(Number Current, Number Target, uint32_t TargetTime)
{
    return Current + (Target - Current) * TimeStep(TargetTime);
};

inline Number LimitPi(Number Value){
    while (Value > PI)
        Value -= 2*PI;
    while (Value < -PI)
        Value += 2*PI;
    return Value;
}
