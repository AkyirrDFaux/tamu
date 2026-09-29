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

// High 32 bits of the signed 64-bit product a*b (Hacker's Delight "mulhs").
// The high halves are split off *signed* deliberately: an all-unsigned decomposition needs
// 33 bits for the cross terms aH*bL + aL*bH and silently drops the carry out of that sum.
// (That is exactly what an earlier version did, returning a value 2^16 too low whenever both
// operands were negative. FixedMul32 only reads the low 16 bits of the result, so it was
// unaffected - but anyone else calling this would have been off by 65536.)
inline int32_t MulHigh32(int32_t a, int32_t b)
{
    const uint32_t aL = (uint32_t)a & 0xFFFFu;
    const uint32_t bL = (uint32_t)b & 0xFFFFu;
    const int32_t aH = a >> 16;
    const int32_t bH = b >> 16;

    const uint32_t lowProduct = aL * bL;
    // Both intermediates fit in 32 bits: |aH*bL| <= 2147450880 and |aL*bH| <= 2147385345, so
    // adding the (< 2^16) carry can never reach past INT32_MIN/MAX.
    const int32_t t = aH * (int32_t)bL + (int32_t)(lowProduct >> 16);
    const int32_t w1 = (t & 0xFFFF) + aL * bH;

    return aH * bH + (t >> 16) + (w1 >> 16);
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
// quotient fits in 32 bits. noinline so the 48-iteration loop is emitted ONCE and
// shared by every fixed-point division call site (the DAS's flash budget).
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
    // Constructs a Number from a plain int value. Needed because int32_t is `long` on the
    // embedded toolchains, so without it an `int` argument (or anything that promotes to one,
    // like uint16_t) is ambiguous between the long and unsigned long overloads.
    // On a 32-bit host int32_t already IS int, where this would be a redefinition of the
    // first constructor - the native numeric tests define TAMU_INT32_IS_INT to skip it.
#ifndef TAMU_INT32_IS_INT
    constexpr Number(int NewValue) : Value(NewValue << DECIMAL) {}
#endif

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
    // Example: 1.5 becomes 2, 1.4 becomes 1, -1.4 becomes -1
    inline int32_t RoundToInt() const
    {
        // Adding half and flooring is correct for both signs (a plain `Value >> 16` would
        // floor, and subtracting half for negatives rounded every negative down by one).
        // The add is guarded so a Value within half of the Q16.16 limit cannot overflow the
        // 32-bit intermediate (there is no representable fraction left to round there).
        constexpr int32_t half = 1 << (DECIMAL - 1);
        if (Value > INT32_MAX - half || Value < INT32_MIN + half)
            return Value >> DECIMAL;
        return (Value + half) >> DECIMAL;
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
// Used by the Vector/Matrix support only; compiled out on SCALAR_ONLY targets.
#ifndef SCALAR_ONLY
#ifdef NUMBER_ONLY_32BIT
// 32-bit-only variant: decompose into integer sqrt then shift.
// For 16.16 fixed point: sqrt(x_16.16) = isqrt32(x) << 8.
inline uint32_t isqrt32(uint32_t x)
{
    if (x == 0) return 0;
    uint32_t result = 0;
    uint32_t bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit != 0)
    {
        if (x >= result + bit)
        {
            x -= result + bit;
            result = (result >> 1) + bit;
        }
        else
        {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}
inline Number sqrt(Number A)
{
    if (A.Value <= 0) return Number(0);
    // isqrt32 of the raw value shifted up carries only 8 fractional bits, so the result can
    // be off by ~2.3 % (the integer sqrt drops a whole unit, worth 256 raw units). One Newton
    // step in fixed point, r = (r + A/r) / 2, brings that to the Q16.16 quantisation limit.
    int32_t r = (int32_t)(isqrt32((uint32_t)A.Value) << 8);
    if (r == 0) return Number(0);
    return Number::FromRaw((r + FixedDiv32(A.Value, r)) >> 1);
}
#else
// 64-bit variant: full precision, used by ESP32 core.
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
#endif
#endif // SCALAR_ONLY

#define RAW_PI 205887
// sqrt(2) in 16.16 - the log range-reduction split point (see log()).
#define RAW_SQRT2 92682
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
#define RAW_SIN_P 14746    // Fixed-point 16.16 for 0.225 (improved-parabola correction)

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

    // Improved parabola: the plain Bx + Cx|x| has a ~5.6% amplitude error (visible as an
    // oversized shape from a rotated transform), corrected to ~0.1% by
    // y' = P*(y*|y| - y) + y.
    int32_t y = P1 - P2;
    int32_t yAbs = (y < 0) ? -y : y;
    int32_t corr = FixedMul32(RAW_SIN_P, FixedMul32(y, yAbs) - y);

    return Number::FromRaw(y + corr);
}

// Fixed-point cosine computed as sine of (angle + PI/2)
inline Number cos(Number X)
{
    // Directly add the raw value to avoid a Number constructor call
    return sin(Number::FromRaw(X.Value + RAW_HALF_PI));
}

#define RAW_ATAN_C0 (-3047)  // Q16.16 of -0.0464964749
#define RAW_ATAN_C1 10441    // Q16.16 of  0.15931422
#define RAW_ATAN_C2 (-21471) // Q16.16 of -0.327622764

// Fixed-point atan2(Y, X) returning the angle in radians, in [-PI, PI].
// Uses the standard rational reduction a = min(|X|,|Y|) / max(|X|,|Y|) with a cubic in a^2,
// which holds the worst-case error to ~3e-4 rad. (The previous form was off by up to
// 0.071 rad - 4 degrees - around +-163 deg, which skewed the Polygon/Star sector lookup in
// the LED renderer.)
inline Number atan2(Number Y, Number X)
{
    const int32_t ax = abs(X).Value;
    const int32_t ay = abs(Y).Value;
    if (ax == 0 && ay == 0)
        return Number(0);

    const bool yLarger = ay > ax;
    const int32_t hi = yLarger ? ay : ax;
    const int32_t lo = yLarger ? ax : ay;
    const Number a = Number::FromRaw(lo) / Number::FromRaw(hi); // in [0, 1]

    const Number s = a * a;
    Number t = Number::FromRaw(RAW_ATAN_C0) * s + Number::FromRaw(RAW_ATAN_C1);
    t = t * s + Number::FromRaw(RAW_ATAN_C2);
    Number r = (t * s) * a + a;

    if (yLarger)
        r = Number::FromRaw(RAW_HALF_PI) - r;
    if (X.Value < 0)
        r = GetPI() - r;
    if (Y.Value < 0)
        r = -r;
    return r;
}

// Fixed-point natural logarithm via range reduction plus a Horner polynomial (returns 0 for non-positive input)
inline Number log(Number x)
{
    if (x.Value <= 0)
        return Number::FromRaw(0);

    int32_t val = x.Value;
    int32_t log2_count = 0;

    // 1. Range Reduction: bring the mantissa into [1, 2)...
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

    // ...then centre it on 1 by folding the upper half back down, giving [sqrt(1/2),
    // sqrt(2)) and |y| <= 0.415 instead of 1. The Horner series below is a 4-term truncation,
    // so its error grows quickly with |y|: without this the worst case (just below a power of
    // two) was 0.109 absolute, ~16 % relative at ln 2. Centring cuts it to ~0.002, and the
    // powers of two stay exact because either way the mantissa lands on y = 0.
    if (val >= RAW_SQRT2)
    {
        val >>= 1;
        log2_count++;
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

// Base-10 logarithm (natural log scaled by 1/ln 10).
inline Number log10(Number x) { return log(x) / Number::FromRaw(150902); } // 1/ln10 in 16.16

// 10^y for any finite y (multiplies only, so 32-bit targets pull in no libgcc helper).
// Saturates at the Q16.16 ceiling (~32767) and returns ~0 for very negative y.
//
// Range reduction: y = n + f with n = floor(y) in 0..4 and f in [0,1). Then
// 10^f = 2^u with u = f*log2(10) = i + v (i in 0..3, v in [0,1)), and 2^v is a quintic.
// This replaces the 16-entry 10^(2^-i) table with five multiplies: no 64-byte table, fewer
// iterations (5 vs 16) and a worst-case error of ~0.013 % over the whole range (the table
// gave 16-bit resolution near the low bits).
#define RAW_LOG2_10 217706 // Q16.16 of log2(10) = 3.321928
inline Number pow10(Number y)
{
    if (y.Value < 0) return Number(1) / pow10(-y);
    // 10^4.5 = 31623, safely inside the Q16.16 range; saturate beyond it.
    if (y.Value >= 294912) return Number::FromRaw(31623 << DECIMAL);

    const int32_t n = y.Value >> DECIMAL;               // 0..4
    const int32_t frac = y.Value & 0xFFFF;              // Q16.16 fraction, [0,1)
    const int32_t u = FixedMul32(frac, RAW_LOG2_10);    // f * log2(10)
    const int32_t v = u & 0xFFFF;                       // fraction of u

    // 2^v for v in [0,1): Horner with the coefficients (ln 2)^k / k!.
    int32_t r = FixedMul32(87, v) + 630;
    r = FixedMul32(r, v) + 3638;
    r = FixedMul32(r, v) + 15743;
    r = FixedMul32(r, v) + 45426;
    r = FixedMul32(r, v) + 65536;
    r <<= (u >> DECIMAL);                               // * 2^(integer part of u)

    Number result = Number::FromRaw(r);
    int32_t k = n;
    while (k-- > 0) result = result * Number(10);
    return result;
}

// Returns the smaller of `A` and `B` (fixed-point)
inline Number min(Number A, Number B) { return (A.Value < B.Value) ? A : B; }
// Returns the larger of `A` and `B` (fixed-point)
inline Number max(Number A, Number B) { return (A.Value > B.Value) ? A : B; }

// Internal helper to get a raw pseudo-random 32-bit integer. The state is a
// function-local static so every translation unit shares one sequence (a
// namespace-scope static in a header would give each TU its own RNG).
// PRNG state shared by SeedRand() and RawRand(). All TUs that include this
// header share the same object because it's declared `inline` (C++17).
inline uint32_t &RandState()
{
    static uint32_t state = 1;
    return state;
}

// Seed the PRNG from a platform entropy source. Call once at boot before any
// RawRand() usage. The seed is XOR'd into the state so calling multiple times
// (e.g. from different subsystems) mixes entropy rather than resetting it.
inline void SeedRand(uint32_t seed)
{
    RandState() ^= seed;
}

inline uint32_t RawRand()
{
    uint32_t &next_rand = RandState();
    next_rand = next_rand * 1103515245 + 12345;
    return next_rand;
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

