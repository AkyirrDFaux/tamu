#pragma once

#include <cstdint>

#define sq(a) ((a) * (a))

#define DECIMAL 16

// ---------------------------------------------------------------------------
// 32-bit-only Q16.16 arithmetic helpers, used when NUMBER_ONLY_32BIT is defined.
// They perform the multiply and divide without any 64-bit arithmetic, so no libgcc
// helpers (__muldi3 / __divdi3 and friends) are pulled in on 32-bit microcontrollers.
// Both are exact for results that fit in 32 bits.
// ---------------------------------------------------------------------------

// High 32 bits of the signed 64-bit product a*b (Hacker's Delight "mulhs"), computed
// with 16-bit splitting and well-defined unsigned wraparound.
inline int32_t MulHigh32(int32_t a, int32_t b)
{
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    uint32_t aH = ua >> 16, aL = ua & 0xFFFF;
    uint32_t bH = ub >> 16, bL = ub & 0xFFFF;

    uint32_t t = aL * bL;                                  // low product
    uint32_t m = aL * bH + aH * bL;                        // cross terms (wraps mod 2^32)
    uint32_t hi = aH * bH + (m >> 16) + (((m & 0xFFFF) + (t >> 16)) >> 16);

    uint32_t r = hi;
    if (a < 0) r -= ub;                                    // sign correction
    if (b < 0) r -= ua;
    return (int32_t)r;
}

// (a * b) >> 16 as Q16.16: bits [16..47] of the signed 64-bit product, 32-bit only.
inline int32_t FixedMul32(int32_t a, int32_t b)
{
    uint32_t low = (uint32_t)a * (uint32_t)b;              // low 32 bits of the product
    uint32_t high = (uint32_t)MulHigh32(a, b);             // bits [32..63]
    return (int32_t)((high << 16) + (low >> 16));
}

// (a << 16) / b as Q16.16, 32-bit only (bit-by-bit long division over the 48-bit
// dividend). Returns 0 on divide-by-zero, matching the 64-bit version. Exact when the
// quotient fits in 32 bits.
inline int32_t FixedDiv32(int32_t a, int32_t b)
{
    if (b == 0) return 0;

    uint32_t ma = (uint32_t)a, mb = (uint32_t)b;
    ma = (a < 0) ? (uint32_t)(0u - ma) : ma;               // |a|
    mb = (b < 0) ? (uint32_t)(0u - mb) : mb;               // |b|

    uint32_t rem = 0, q = 0;
    for (int i = 0; i < 48; i++)                           // dividend = |a| << 16
    {
        uint32_t bit = (i < 32) ? (ma >> 31) : 0;
        ma <<= 1;
        rem = (rem << 1) | bit;
        q = (q << 1) | ((rem >= mb) ? 1u : 0u);
        if (rem >= mb) rem -= mb;
    }

    uint32_t r = q;
    if ((a < 0) != (b < 0)) r = 0u - r;                    // apply sign
    return (int32_t)r;
}

class Number
{
public:
    int32_t Value = 0;

    Number() = default;
    // Constructs a Number from an integer value, scaling it to 16.16 fixed-point.
    // (int32_t is `long` on the embedded toolchains, so a separate `int` overload keeps
    // calls with plain int/uint16_t arguments unambiguous.)
    constexpr Number(int32_t NewValue) : Value(NewValue << DECIMAL) {}
    // Constructs a Number from an unsigned integer value
    constexpr Number(uint32_t NewValue) : Value(int32_t(NewValue << DECIMAL)) {}
    // Constructs a Number from a plain int value
    constexpr Number(int NewValue) : Value(NewValue << DECIMAL) {}

    // Wraps an already-scaled raw 16.16 value without re-scaling
    static constexpr Number FromRaw(int32_t raw)
    {
        Number n;
        n.Value = raw;
        return n;
    }

    // Converts the fixed-point value to an integer by truncating (floor for negatives)
    inline int32_t ToInt() const
    {
        return Value >> DECIMAL;
    }

    // Round to Nearest (Most Accurate)
    // Example: 1.5 becomes 2, 1.4 becomes 1
    inline int32_t RoundToInt() const
    {
        if (Value >= 0)
            return (Value + (1 << (DECIMAL - 1))) >> DECIMAL;
        else
            return (Value - (1 << (DECIMAL - 1))) >> DECIMAL;
    }

    // Compound add: adds `Other` in fixed-point
    inline Number &operator+=(const Number &Other)
    {
        Value += Other.Value;
        return *this;
    }
    // Compound subtract: subtracts `Other` in fixed-point
    inline Number &operator-=(const Number &Other)
    {
        Value -= Other.Value;
        return *this;
    }
    // Addition of two Numbers (fixed-point)
    inline Number operator+(const Number &Other) const { return Number(*this) += Other; }
    // Subtraction of two Numbers (fixed-point)
    inline Number operator-(const Number &Other) const { return Number(*this) -= Other; }
    // Unary negation
    inline Number operator-() const
    {
        Number Result;
        Result.Value = -Value;
        return Result;
    }

    // Fixed-point multiplication (result kept in 16.16)
    inline Number operator*(const Number &Other) const
    {
        Number Result;
#ifdef NUMBER_ONLY_32BIT
        Result.Value = FixedMul32(Value, Other.Value);
#else
        Result.Value = int32_t((int64_t(Value) * Other.Value) >> DECIMAL);
#endif
        return Result;
    }

    // Fixed-point division (returns 0 on divide-by-zero)
    inline Number operator/(const Number &Other) const
    {
        Number Result;
#ifdef NUMBER_ONLY_32BIT
        Result.Value = FixedDiv32(Value, Other.Value);
#else
        if (Other.Value == 0)
        {
            Result.Value = 0;
            return Result;
        }
        Result.Value = int32_t((int64_t(Value) << DECIMAL) / Other.Value);
#endif
        return Result;
    }

    // Integer Overloads (replaces templates)
    inline Number operator+(int32_t Other) const { return *this + Number(Other); }
    inline Number operator-(int32_t Other) const { return *this - Number(Other); }
    inline Number operator*(int32_t Other) const { return *this * Number(Other); }
    inline Number operator/(int32_t Other) const { return *this / Number(Other); }

    // Equality: compares raw fixed-point values
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

// Returns the absolute value of `A` (fixed-point)
inline Number abs(Number A)
{
    Number Result;
    Result.Value = (A.Value < 0) ? -A.Value : A.Value;
    return Result;
}

// Fixed-point square root via binary digit-by-digit extraction (returns 0 for non-positive input).
// NOTE: uses uint64_t intermediates by necessity (48+ bit working value); it is only linked
// where called, so keep it out of flash-constrained node images unless 64-bit helpers are
// already present.
inline Number sqrt(Number A)
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
// Single shared PI instance: a plain namespace-scope `static const Number` would be
// duplicated (with internal linkage) in every translation unit including this header.
inline const Number &GetPI()
{
    static const Number pi = Number::FromRaw(RAW_PI);
    return pi;
}
#define RAW_TWO_PI 411774
#define RAW_HALF_PI 102943 // PI / 2 in 16.16
#define RAW_SIN_B 83443    // Fixed-point 16.16 for 4/pi
#define RAW_SIN_C 26561    // Fixed-point 16.16 for 4/pi^2

// Fixed-point sine approximation using a parabola, after reducing the angle into [-PI, PI]
inline Number sin(Number X)
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
    // FixedMul32 computes (a*b)>>16 exactly (32-bit only), so no 64-bit multiply
    // helper (__muldi3) can be pulled in even without NUMBER_ONLY_32BIT.
    int32_t P1 = FixedMul32(RAW_SIN_B, x);

    // Calculate x * |x|
    int32_t xAbs = (x < 0) ? -x : x;
    int32_t xSquared = FixedMul32(x, xAbs);

    int32_t P2 = FixedMul32(RAW_SIN_C, xSquared);

    return Number::FromRaw(P1 - P2);
}

// Fixed-point cosine computed as sine of (angle + PI/2)
inline Number cos(Number X)
{
    // Directly add the raw value to avoid a Number constructor call
    return sin(Number::FromRaw(X.Value + RAW_HALF_PI));
}

// Fixed-point atan2(Y, X) returning the angle in radians (approximate)
inline Number atan2(Number Y, Number X)
{
    // This is not very accurate
    int32_t Sign = Y > 0 ? 1 : -1;
    if (X < 0)
        return Sign * (3 * GetPI() / 4 - GetPI() / 4 * ((X + abs(Y)) / (abs(Y) - X)));
    else
        return Sign * (GetPI() / 4 - GetPI() / 4 * ((X - abs(Y)) / (X + abs(Y))));
};

// Fixed-point natural logarithm via range reduction plus a Horner polynomial (returns 0 for non-positive input)
inline Number log(Number x)
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

    // We compute from the inside out (FixedMul32 = exact (a*b)>>16, 32-bit only):
    // a = (1/3 - y/4)
    int32_t a = 0x5555 - (y >> 2);
    // b = (1/2 - y*a)
    int32_t b = 0x8000 - FixedMul32(y, a);
    // c = (1 - y*b)
    int32_t c = 0x10000 - FixedMul32(y, b);
    // ln_m = y * c
    int32_t ln_m = FixedMul32(y, c);

    // 3. Final Reconstruction
    const int32_t LN2 = 45426;

    return Number::FromRaw((log2_count * LN2) + ln_m);
}

// Returns the smaller of `A` and `B` (fixed-point)
inline Number min(Number A, Number B) { return (A.Value < B.Value) ? A : B; }
// Returns the larger of `A` and `B` (fixed-point)
inline Number max(Number A, Number B) { return (A.Value > B.Value) ? A : B; }

// Internal helper to get a raw pseudo-random 32-bit integer. The state is a
// function-local static so every translation unit shares one sequence (a
// namespace-scope static in a header would give each TU its own RNG).
inline uint32_t RawRand()
{
    static uint32_t next_rand = 1; // Seed this with AnalogRead or CurrentTime
    next_rand = next_rand * 1103515245 + 12345;
    return next_rand;
}

// Returns a Number between 0.0 and 1.0
inline Number RandomPercent()
{
    // We take the top 16 bits of the random result and place them
    // into the fractional part of our 16.16 Number.
    return Number::FromRaw(RawRand() >> 16);
}

// Clamps `Value` into the range [0, 1]
inline Number LimitZeroToOne(Number Value)
{
    return min(max(Value, Number(0)), Number(1));
};

// Clamps an integer into the byte range [0, 255]
inline uint8_t LimitByte(int Value)
{
    return min(max(Value, 0), 255).ToInt();
};

// Converts an 8-bit value (0-255) to a Number fraction (0.0-1.0)
inline Number ByteToPercent(uint8_t Value)
{
    return LimitZeroToOne(Number(Value) / 255);
};

// Converts a Number fraction (0.0-1.0) to an 8-bit value (0-255).
// Scales before truncating so fractions below 1.0 still map correctly
// (e.g. 0.5 -> 127, not 0).
inline uint8_t PercentToByte(Number Value)
{
    return LimitByte((Value * 255).ToInt());
};

// Multiplies two byte-percent values (0-255 each) and returns the byte result
inline uint8_t MultiplyBytePercentByte(uint8_t ByteValue, uint8_t Percent)
{
    return (uint8_t)(((int)ByteValue * (int)Percent) / 255);
};

// Wraps `Value` into the range [-PI, PI]
inline Number LimitPi(Number Value)
{
    while (Value > GetPI())
        Value -= 2 * GetPI();
    while (Value < -GetPI())
        Value += 2 * GetPI();
    return Value;
}

