#define sq(a) ((a) * (a))

#if USE_FIXED_POINT == 1
#define DECIMAL 16

class Number
{
public:
    int32_t Value = 0;

    Number() = default;
    constexpr Number(int32_t NewValue) : Value(NewValue << DECIMAL) {}
    constexpr Number(uint32_t NewValue) : Value(int32_t(NewValue << DECIMAL)) {}
    constexpr Number(int NewValue) : Value(NewValue << DECIMAL) {}

    static constexpr Number FromRaw(int32_t raw)
    {
        Number n;
        n.Value = raw;
        return n;
    }

    inline int32_t ToInt() const
    {
        return Value >> DECIMAL;
    }

    // 2. Round to Nearest (Most Accurate)
    // Example: 1.5 becomes 2, 1.4 becomes 1
    inline int32_t RoundToInt() const
    {
        if (Value >= 0)
            return (Value + (1 << (DECIMAL - 1))) >> DECIMAL;
        else
            return (Value - (1 << (DECIMAL - 1))) >> DECIMAL;
    }

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

    // Integer Overloads (replaces templates)
    inline Number operator+(int32_t Other) const { return *this + Number(Other); }
    inline Number operator-(int32_t Other) const { return *this - Number(Other); }
    inline Number operator*(int32_t Other) const { return *this * Number(Other); }
    inline Number operator/(int32_t Other) const { return *this / Number(Other); }

    inline bool operator==(const Number &Other) const { return Value == Other.Value; }
    inline bool operator!=(const Number &Other) const { return Value != Other.Value; }
    inline bool operator<(const Number &Other) const { return Value < Other.Value; }
    inline bool operator>(const Number &Other) const { return Value > Other.Value; }
    inline bool operator<=(const Number &Other) const { return Value <= Other.Value; }
    inline bool operator>=(const Number &Other) const { return Value >= Other.Value; }

    // Integer Comparisons
    inline bool operator<(int32_t Other) const { return *this < Number(Other); }
    inline bool operator>(int32_t Other) const { return *this > Number(Other); }
    inline bool operator<=(int32_t Other) const { return *this <= Number(Other); }
    inline bool operator>=(int32_t Other) const { return *this >= Number(Other); }
};

#define N(n) (Number::FromRaw((int32_t)((n) * (1 << DECIMAL))))

// Global Integer Helpers (replaces templates)
inline Number operator+(int32_t A, const Number &B) { return Number(A) + B; }
inline Number operator-(int32_t A, const Number &B) { return Number(A) - B; }
inline Number operator*(int32_t A, const Number &B) { return Number(A) * B; }
inline Number operator/(int32_t A, const Number &B) { return Number(A) / B; }

inline Number abs(Number A)
{
    Number Result;
    Result.Value = (A.Value < 0) ? -A.Value : A.Value;
    return Result;
}

Number sqrt(Number A)
{
    if (A.Value <= 0)
        return Number(0);

    uint64_t val = (uint64_t)A.Value << 16; // Shift up to maintain 16.16 precision
    uint64_t res = 0;
    uint64_t add = (uint64_t)1 << 46; // Start at a high bit

    for (int i = 0; i < 24; i++)
    {
        uint64_t temp = res + add;
        res >>= 1;
        if (val >= temp)
        {
            val -= temp;
            res += add;
        }
        add >>= 2;
    }
    return Number::FromRaw((int32_t)res);
}

#define RAW_PI 205887
static const Number PI = Number::FromRaw(RAW_PI);
#define RAW_TWO_PI 411774
#define RAW_HALF_PI 102943 // PI / 2 in 16.16
#define RAW_SIN_B 83443    // Fixed-point 16.16 for 4/pi
#define RAW_SIN_C 26561    // Fixed-point 16.16 for 4/pi^2

Number sin(Number X)
{
    int32_t x = X.Value;

    // 2. Range reduction: x = x % (2*PI)
    // Note: % is expensive on some RISC-V, but for 32-bit it's usually acceptable.
    // If you only pass small values, an 'if' is faster.
    x %= RAW_TWO_PI;

    if (x > RAW_PI)
        x -= RAW_TWO_PI;
    else if (x < -RAW_PI)
        x += RAW_TWO_PI;

    // 3. Parabola approximation: y = Bx + Cx|x|
    // We use int64_t to prevent overflow during the multiplication before shifting back
    int32_t P1 = (int32_t)(((int64_t)RAW_SIN_B * x) >> 16);

    // Calculate x * |x|
    int32_t xAbs = (x < 0) ? -x : x;
    int32_t xSquared = (int32_t)(((int64_t)x * xAbs) >> 16);

    int32_t P2 = (int32_t)(((int64_t)RAW_SIN_C * xSquared) >> 16);

    return Number::FromRaw(P1 - P2);
}

inline Number cos(Number X)
{
    // Directly add the raw value to avoid a Number constructor call
    return sin(Number::FromRaw(X.Value + RAW_HALF_PI));
}

Number atan2(Number Y, Number X)
{
    // This is not very accurate
    int32_t Sign = Y > 0 ? 1 : -1;
    if (X < 0)
        return Sign * (3 * PI / 4 - PI / 4 * ((X + abs(Y)) / (abs(Y) - X)));
    else
        return Sign * (PI / 4 - PI / 4 * ((X - abs(Y)) / (X + abs(Y))));
};

Number log(Number x)
{
    if (x.Value <= 0)
        return Number::FromRaw(0);

    int32_t val = x.Value;
    int32_t log2_count = 0;

    // 1. Range Reduction
    if (val >= 0x10000)
    {
        while (val >= 0x20000)
        {
            val >>= 1;
            log2_count++;
        }
    }
    else
    {
        while (val < 0x10000)
        {
            val <<= 1;
            log2_count--;
        }
    }

    // 2. Polynomial Approximation using Horner's Method
    // Target: y - y²/2 + y³/3 - y⁴/4  =>  y * (1 - y * (1/2 - y * (1/3 - y/4)))
    int32_t y = val - 0x10000;

    // Constants in 16.16 fixed point
    // 1/4 = 0x4000, 1/3 = 0x5555, 1/2 = 0x8000, 1 = 0x10000

    // We compute from the inside out:
    // a = (1/3 - y/4)
    int32_t a = 0x5555 - (y >> 2);
    // b = (1/2 - y*a)
    int32_t b = 0x8000 - (int32_t)(((int64_t)y * a) >> 16);
    // c = (1 - y*b)
    int32_t c = 0x10000 - (int32_t)(((int64_t)y * b) >> 16);
    // ln_m = y * c
    int32_t ln_m = (int32_t)(((int64_t)y * c) >> 16);

    // 3. Final Reconstruction
    const int32_t LN2 = 45426;

    return Number::FromRaw((log2_count * LN2) + ln_m);
}

inline Number min(Number A, Number B) { return (A.Value < B.Value) ? A : B; }
inline Number max(Number A, Number B) { return (A.Value > B.Value) ? A : B; }

static uint32_t _next_rand = 1; // Seed this with AnalogRead or CurrentTime

// Internal helper to get a raw pseudo-random 32-bit integer
inline uint32_t RawRand()
{
    _next_rand = _next_rand * 1103515245 + 12345;
    return _next_rand;
}

// Returns a Number between 0.0 and 1.0
Number RandomPercent()
{
    // We take the top 16 bits of the random result and place them
    // into the fractional part of our 16.16 Number.
    return Number::FromRaw(RawRand() & 0xFFFF);
}

#else
typedef float Number;
#endif

inline Number LimitZeroToOne(Number Value)
{
    return min(max(Value, Number(0)), Number(1));
};

inline uint8_t LimitByte(int Number)
{
    return min(max(Number, 0), 255).ToInt();
};

inline Number ByteToPercent(uint8_t Value)
{
    return LimitZeroToOne(Number(Value) / 255);
};

inline uint8_t PercentToByte(Number Value)
{
    return LimitByte(Value.ToInt() * 255);
};

inline uint8_t MultiplyBytePercentByte(uint8_t Number, uint8_t Percent)
{
    return (uint8_t)(((int)Number * (int)Percent) / 255);
};

inline Number LimitPi(Number Value)
{
    while (Value > PI)
        Value -= 2 * PI;
    while (Value < -PI)
        Value += 2 * PI;
    return Value;
}
