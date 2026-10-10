// Host-side unit tests for the firmware numeric core (Core/Types/*.h).
//
// The numeric core is deliberately free of any device dependency: Number.h only needs
// <cstdint>, and Vector/Matrix/Colour only need Number.h included first. That makes the
// whole thing compilable natively, which is exactly how the trig regression that shipped
// in an earlier beta (a ~5.6 % sine amplitude error) should have been caught.
//
// Compile it twice - once for each real build - because the two differ:
//   * core (ESP32): plain 64-bit multiply/divide and the 64-bit sqrt,
//   * DAS (CH32V003): NUMBER_ONLY_32BIT (FixedMul32/FixedDiv32/isqrt32) and SCALAR_ONLY
//     (no Vector/Matrix at all).
// See run.sh. No framework is used on purpose: the firmware test folder must stay
// buildable by a bare g++, with no network access to fetch Unity.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "Core/Types/Number.h"
#include "Core/Types/Vector.h"
#include "Core/Types/Matrix.h"
#include "Core/Types/Colour.h"

// ---------------------------------------------------------------------------
// Tiny assertion framework
// ---------------------------------------------------------------------------

static int GChecks = 0;
static int GFailures = 0;

static double ToD(Number value) { return value.Value / 65536.0; }

// Number(double) is deliberately ambiguous (every integer constructor needs a conversion),
// so doubles go through the N() idiom or this helper.
static Number num(double value) { return Number::FromRaw((int32_t)(value * 65536.0)); }

#define CHECK(cond)                                                                              \
    do                                                                                           \
    {                                                                                            \
        GChecks++;                                                                               \
        if (!(cond))                                                                             \
        {                                                                                        \
            GFailures++;                                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
        }                                                                                        \
    } while (0)

// Asserts that `actual` (a Number, or a raw double) is within `tol` of `expect`.
#define CHECK_NEAR(actual, expect, tol)                                                          \
    do                                                                                           \
    {                                                                                            \
        GChecks++;                                                                               \
        const double a_ = ToD(actual);                                                           \
        const double e_ = (expect);                                                              \
        if (!(std::fabs(a_ - e_) <= (tol)))                                                      \
        {                                                                                        \
            GFailures++;                                                                         \
            std::printf("FAIL %s:%d: %s = %.6f, expected %.6f +/- %.6f\n", __FILE__, __LINE__,   \
                        #actual, a_, e_, (double)(tol));                                         \
        }                                                                                        \
    } while (0)

#define CHECK_EQ(actual, expect)                                                                 \
    do                                                                                           \
    {                                                                                            \
        GChecks++;                                                                               \
        if (!((actual) == (expect)))                                                             \
        {                                                                                        \
            GFailures++;                                                                         \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #actual, #expect);         \
        }                                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// Construction, arithmetic, conversion
// ---------------------------------------------------------------------------

static void TestBasics()
{
    CHECK_EQ(Number(5).Value, 5 << DECIMAL);
    CHECK_EQ(Number(0).Value, 0);
    CHECK_EQ(Number(-1).Value, -(1 << DECIMAL));
    CHECK_EQ(Number::FromRaw(12345).Value, 12345);

    CHECK_EQ(Number(2) + Number(3), Number(5));
    CHECK_EQ(Number(2) - Number(5), Number(-3));
    CHECK_EQ(Number(-2) - Number(-5), Number(3));
    CHECK_EQ(-Number(4), Number(-4));

    CHECK_NEAR(Number(7) / Number(2), 3.5, 1e-6);
    CHECK_NEAR(Number(1) / Number(3), 1.0 / 3.0, 1e-4);
    CHECK_NEAR(Number(-7) / Number(2), -3.5, 1e-6);
    CHECK_NEAR(Number(7) / Number(-2), -3.5, 1e-6);

    // Divide by zero is defined as zero rather than trapping.
    CHECK_EQ(Number(5) / Number(0), Number(0));
    CHECK_EQ(Number(0) / Number(0), Number(0));

    CHECK_NEAR(Number(3) * Number(4), 12.0, 1e-6);
    CHECK_NEAR(Number(-3) * Number(4), -12.0, 1e-6);
    CHECK_NEAR(N(1.5) * N(1.5), 2.25, 1e-6);
    // 0.1 is not representable in Q16.16: N(0.1) is 6553/65536. The multiply itself must be
    // exact for the operand it was actually given (6553 * 10 = 65530).
    CHECK_EQ(N(0.1) * Number(10), Number::FromRaw(65530));

    // Mixed integer overloads.
    CHECK_EQ(Number(5) + 3, Number(8));
    CHECK_EQ(Number(5) - 3, Number(2));
    CHECK_EQ(Number(5) * 3, Number(15));
    CHECK_NEAR(Number(5) / 2, 2.5, 1e-6);

    // ToInt truncates toward negative infinity; RoundToInt is half-away-from... half-up.
    CHECK_EQ(N(1.9).ToInt(), 1);
    CHECK_EQ(N(-1.1).ToInt(), -2);
    CHECK_EQ(N(1.5).RoundToInt(), 2);
    CHECK_EQ(N(1.4).RoundToInt(), 1);
    CHECK_EQ(N(-1.4).RoundToInt(), -1);
    CHECK_EQ(N(2.5).RoundToInt(), 3);
    CHECK_EQ(N(-2.5).RoundToInt(), -2); // documented "add half and floor" behaviour

    // The rounding guard must not overflow near the Q16.16 limits.
    CHECK_EQ(Number::FromRaw(INT32_MAX).RoundToInt(), INT32_MAX >> DECIMAL);
    CHECK_EQ(Number::FromRaw(INT32_MIN).RoundToInt(), INT32_MIN >> DECIMAL);

    CHECK_EQ(abs(Number(-3)), Number(3));
    CHECK_EQ(abs(Number(3)), Number(3));
    CHECK_EQ(min(Number(2), Number(7)), Number(2));
    CHECK_EQ(max(Number(2), Number(7)), Number(7));

    CHECK_EQ(LimitZeroToOne(Number(-1)), Number(0));
    CHECK_EQ(LimitZeroToOne(Number(2)), Number(1));
    CHECK_EQ(LimitZeroToOne(N(0.5)), N(0.5));

    CHECK_EQ(LimitByte(-5), 0);
    CHECK_EQ(LimitByte(300), 255);
    CHECK_EQ(LimitByte(128), 128);

    CHECK_NEAR(ByteToPercent(0), 0.0, 1e-6);
    CHECK_NEAR(ByteToPercent(255), 1.0, 1e-6);
    CHECK_NEAR(ByteToPercent(128), 0.50196, 1e-3);

    // The two sides of the SCALAR_ONLY builds agree on the default state.
    CHECK_EQ((Number() + Number()).Value, 0);
}

// ---------------------------------------------------------------------------
// 32-bit-only fixed-point primitives (used by the DAS)
// ---------------------------------------------------------------------------

static void TestFixedPrimitives()
{
    const int32_t values[] = {0,       1,        -1,       2,       -2,       100,      -100,
                              65536,   12345,    -12345,   1000000, -1000000, (1 << 20), -(1 << 20),
                              46341,   -46341,   46340,    -46340,  252645135, -252645135};

    // FixedMul32 must equal the exact ((int64_t)a * b) >> 16.
    for (int32_t Lhs : values)
    {
        for (int32_t Rhs : values)
        {
            const int32_t expected = (int32_t)(((int64_t)Lhs * Rhs) >> DECIMAL);
            const int32_t got = FixedMul32(Lhs, Rhs);
            if (got != expected)
            {
                GFailures++;
                std::printf("FAIL FixedMul32(%d, %d) = %d, expected %d\n", Lhs, Rhs, got, expected);
            }
            GChecks++;
        }
    }

    // MulHigh32 must equal the high 32 bits of the exact product.
    for (int32_t Lhs : values)
    {
        for (int32_t Rhs : values)
        {
            const int32_t expected = (int32_t)(((int64_t)Lhs * (int64_t)Rhs) >> 32);
            const int32_t got = MulHigh32(Lhs, Rhs);
            if (got != expected)
            {
                GFailures++;
                std::printf("FAIL MulHigh32(%d, %d) = %d, expected %d\n", Lhs, Rhs, got, expected);
            }
            GChecks++;
        }
    }

    // FixedDiv32 must equal the exact (a << 16) / b, truncating toward zero, and 0 for b == 0.
    for (int32_t Lhs : values)
    {
        for (int32_t Rhs : values)
        {
            const int32_t expected = Rhs == 0 ? 0 : (int32_t)(((int64_t)Lhs << DECIMAL) / Rhs);
            const int32_t got = FixedDiv32(Lhs, Rhs);
            if (got != expected)
            {
                GFailures++;
                std::printf("FAIL FixedDiv32(%d, %d) = %d, expected %d\n", Lhs, Rhs, got, expected);
            }
            GChecks++;
        }
    }
    CHECK_EQ(FixedDiv32(5 << DECIMAL, 0), 0);

    // Randomised sweep. The hand-picked values above are mostly small; the fixed-point paths
    // are exactly where large magnitudes and mixed signs bite.
    uint32_t state = 0x12345678u;
    int32_t mismatches = 0;
    for (int i = 0; i < 200000; i++)
    {
        state = state * 1103515245u + 12345u;
        const int32_t Lhs = (int32_t)state;
        state = state * 1103515245u + 12345u;
        const int32_t Rhs = (int32_t)state;

        if (MulHigh32(Lhs, Rhs) != (int32_t)(((int64_t)Lhs * Rhs) >> 32)) mismatches++;
        if (FixedMul32(Lhs, Rhs) != (int32_t)(((int64_t)Lhs * Rhs) >> DECIMAL)) mismatches++;
        // Skip quotients that cannot fit in 32 bits: FixedDiv32 is documented to be exact
        // only in that range (and the reference would overflow the cast as well).
        if (Rhs != 0)
        {
            const int64_t Quotient = ((int64_t)Lhs << DECIMAL) / Rhs;
            if (Quotient >= INT32_MIN && Quotient <= INT32_MAX &&
                FixedDiv32(Lhs, Rhs) != (int32_t)Quotient) mismatches++;
        }
    }
    GChecks++;
    if (mismatches != 0)
    {
        GFailures++;
        std::printf("FAIL randomised fixed-point sweep: %d mismatches\n", mismatches);
    }
}

// ---------------------------------------------------------------------------
// sqrt
// ---------------------------------------------------------------------------

#ifndef SCALAR_ONLY

static void TestSqrt()
{
    CHECK_NEAR(sqrt(Number(0)), 0.0, 1e-6);
    CHECK_NEAR(sqrt(Number(-1)), 0.0, 1e-6); // non-positive returns zero, never NaN

    CHECK_NEAR(sqrt(Number(1)), 1.0, 1e-4);
    CHECK_NEAR(sqrt(Number(4)), 2.0, 1e-4);
    CHECK_NEAR(sqrt(Number(9)), 3.0, 1e-4);
    CHECK_NEAR(sqrt(Number(144)), 12.0, 1e-4);
    CHECK_NEAR(sqrt(Number(2)), 1.41421356, 1e-3);
    CHECK_NEAR(sqrt(N(0.25)), 0.5, 1e-3);

    // Sweep the range the renderer actually uses (mask distances, vector norms).
    double worst = 0;
    for (int i = 1; i <= 1000; i++)
    {
        const double x = i * 0.1;
        const double got = ToD(sqrt(num(x)));
        const double want = std::sqrt(x);
        worst = std::max(worst, std::fabs(got - want) / want);
    }
    std::printf("  sqrt max relative error: %.5f%%\n", worst * 100.0);
    CHECK(worst < 0.01);

    // Monotonicity.
    Number prev = Number(0);
    for (int i = 1; i <= 200; i++)
    {
        const Number cur = sqrt(Number(i));
        CHECK(cur >= prev);
        prev = cur;
    }
}
#endif

// ---------------------------------------------------------------------------
// Trigonometry
// ---------------------------------------------------------------------------

static void TestTrig()
{
    const Number pi = GetPI();
    const Number HalfPi = Number::FromRaw(RAW_HALF_PI);

    CHECK_NEAR(pi, 3.14159265, 1e-4);
    CHECK_NEAR(sin(Number(0)), 0.0, 1e-5);
    CHECK_NEAR(sin(pi), 0.0, 2e-3);
    CHECK_NEAR(sin(HalfPi), 1.0, 2e-3);
    CHECK_NEAR(sin(-HalfPi), -1.0, 2e-3);
    CHECK_NEAR(cos(Number(0)), 1.0, 2e-3);
    CHECK_NEAR(cos(pi), -1.0, 2e-3);
    CHECK_NEAR(cos(HalfPi), 0.0, 2e-3);

    // The 45-degree cell of the rotation matrix is what the improved parabola fixed.
    // The plain Bx + Cx|x| was ~5.6 % high here; guard it explicitly.
    CHECK_NEAR(sin(Number::FromRaw(RAW_PI / 4)), 0.70710678, 0.02);
    CHECK_NEAR(cos(Number::FromRaw(RAW_PI / 4)), 0.70710678, 0.02);

    // Odd/even symmetry. The tolerance is a few Q16.16 LSBs, not zero: fixed-point multiply
    // floors (arithmetic shift), so f(-x) is not the exact negation of f(x) when a product has
    // a fractional part. This is inherent to the fixed-point representation, not a defect.
    for (int deg = -170; deg <= 170; deg += 10)
    {
        const Number Lhs = Number::FromRaw((int32_t)((double)deg * (RAW_PI / 180.0)));
        CHECK_NEAR(sin(-Lhs), -ToD(sin(Lhs)), 1e-4);
        CHECK_NEAR(cos(-Lhs), ToD(cos(Lhs)), 1e-4);
    }

    // Accuracy across a full turn, and periodicity.
    double WorstSin = 0;
    double WorstCos = 0;
    const int steps = 3600;
    for (int i = 0; i <= steps; i++)
    {
        const double rad = (i * 2.0 * 3.14159265358979) / steps;
        const Number Lhs = num(rad);
        WorstSin = std::max(WorstSin, std::fabs(ToD(sin(Lhs)) - std::sin(rad)));
        WorstCos = std::max(WorstCos, std::fabs(ToD(cos(Lhs)) - std::cos(rad)));
    }
    std::printf("  sin max absolute error: %.5f\n", WorstSin);
    std::printf("  cos max absolute error: %.5f\n", WorstCos);
    CHECK(WorstSin < 0.005);
    CHECK(WorstCos < 0.005);

    // sin(x + 2*PI) == sin(x).
    for (int deg = -180; deg <= 180; deg += 30)
    {
        const Number Lhs = Number::FromRaw((int32_t)((double)deg * (RAW_PI / 180.0)));
        CHECK_NEAR(sin(Lhs + Number::FromRaw(RAW_TWO_PI)), ToD(sin(Lhs)), 1e-3);
    }

    // Range reduction for large arguments: the raw value is taken modulo 2*PI, so a large
    // angle must still land exactly on an axis. This is what keeps a long-running script
    // (an angle that keeps accumulating) from losing precision.
    CHECK_NEAR(sin(Number::FromRaw(RAW_TWO_PI * 50)), 0.0, 5e-3);
    CHECK_NEAR(cos(Number::FromRaw(RAW_TWO_PI * 50)), 1.0, 5e-3);
    CHECK_NEAR(sin(Number::FromRaw(RAW_HALF_PI + RAW_TWO_PI * 1000)), 1.0, 5e-3);
    CHECK_NEAR(cos(Number::FromRaw(RAW_TWO_PI * 999)), 1.0, 5e-3);
    CHECK_NEAR(sin(Number::FromRaw(-(RAW_TWO_PI * 37))), 0.0, 5e-3);

    // Amplitude must be ~1 everywhere (the shape the renderer depends on).
    double MaxAmp = 0;
    for (int i = 0; i <= 360; i++)
    {
        const Number Lhs = Number::FromRaw((int32_t)((double)i * (RAW_PI / 180.0)));
        MaxAmp = std::max(MaxAmp, std::fabs(ToD(sin(Lhs))));
    }
    std::printf("  sin peak amplitude: %.5f\n", MaxAmp);
    CHECK(std::fabs(MaxAmp - 1.0) < 0.005);
}

static void TestAtan2()
{
    const Number pi = GetPI();

    // Exact quadrant boundaries (X = 0 is the tricky one, it must not divide by zero).
    CHECK_NEAR(atan2(Number(0), Number(1)), 0.0, 1e-4);
    CHECK_NEAR(atan2(Number(1), Number(0)), ToD(pi) / 2, 1e-4);
    CHECK_NEAR(atan2(Number(-1), Number(0)), -ToD(pi) / 2, 1e-4);
    CHECK_NEAR(atan2(Number(0), Number(-1)), ToD(pi), 1e-4); // +PI, as std::atan2
    CHECK_NEAR(atan2(Number(0), Number(0)), 0.0, 1e-6);

    CHECK_NEAR(atan2(Number(1), Number(1)), ToD(pi) / 4, 1e-3);
    CHECK_NEAR(atan2(Number(-1), Number(1)), -ToD(pi) / 4, 1e-3);
    CHECK_NEAR(atan2(Number(1), Number(-1)), 3 * ToD(pi) / 4, 1e-3);
    CHECK_NEAR(atan2(Number(-1), Number(-1)), -3 * ToD(pi) / 4, 1e-3);

    // The rational reduction plus cubic in a^2 is accurate to ~3e-4 rad everywhere. Bound it
    // tightly: the previous form was off by up to 0.071 rad (4 deg), which is what this guards.
    // The comparison is modulo 2*PI so the branch cut at +-PI does not count as an error.
    double worst = 0;
    for (int i = -180; i <= 180; i += 5)
    {
        for (int j = -180; j <= 180; j += 15)
        {
            if (i == 0 && j == 0) continue;
            const double y = i / 180.0 * 3.14159265;
            const double x = j / 180.0 * 3.14159265;
            double diff = ToD(atan2(num(y), num(x))) - std::atan2(y, x);
            while (diff > 3.14159265358979) diff -= 2 * 3.14159265358979;
            while (diff < -3.14159265358979) diff += 2 * 3.14159265358979;
            worst = std::max(worst, std::fabs(diff));
        }
    }
    std::printf("  atan2 max absolute error: %.6f rad\n", worst);
    CHECK(worst < 0.001);
}

// ---------------------------------------------------------------------------
// log / log10 / pow10
// ---------------------------------------------------------------------------

static void TestLogExp()
{
    CHECK_NEAR(log(Number(1)), 0.0, 1e-4);
    CHECK_NEAR(log(Number(0)), 0.0, 1e-6);
    CHECK_NEAR(log(Number(-1)), 0.0, 1e-6);
    CHECK_NEAR(log(N(2.718281828)), 1.0, 3e-3);
    CHECK_NEAR(log(Number(10)), 2.302585, 2e-3);

    // Exactly at the top of the reduced range (y = 0) the series is exact, so every power of
    // two is exact. The centred reduction keeps that true (either branch lands on y = 0).
    for (int k = 0; k <= 12; k++)
        CHECK_NEAR(log(Number(1 << k)), k * 0.69314718, 1e-4);

    // The centred range reduction ([sqrt(1/2), sqrt(2)) so |y| <= 0.415) holds the 4-term
    // series' worst-case error to ~0.002. Without it the error reached 0.109 just below each
    // power of two (~16 % relative at ln 2), which fed the DAS LDR lux chain through log10.
    CHECK_NEAR(log10(Number(1)), 0.0, 1e-4);
    CHECK_NEAR(log10(Number(10)), 1.0, 1e-3);
    CHECK_NEAR(log10(Number(100)), 2.0, 1e-3);
    CHECK_NEAR(log10(Number(1000)), 3.0, 1e-3);
    CHECK_NEAR(log10(N(0.1)), -1.0, 1e-3);

    double worst = 0;
    for (int i = 1; i <= 2000; i++)
    {
        const double x = i * 0.5;
        worst = std::max(worst, std::fabs(ToD(log(num(x))) - std::log(x)));
    }
    std::printf("  log max absolute error: %.5f\n", worst);
    CHECK(worst < 0.005);

    // Monotonicity holds despite the truncation, and the LDR lux chain depends on it.
    Number PrevLog = log(num(0.01));
    for (int i = 1; i <= 2000; i++)
    {
        const Number cur = log(num(i * 0.5));
        CHECK(cur >= PrevLog);
        PrevLog = cur;
    }

    CHECK_NEAR(pow10(Number(0)), 1.0, 1e-4);
    CHECK_NEAR(pow10(Number(1)), 10.0, 1e-3);
    CHECK_NEAR(pow10(Number(2)), 100.0, 1e-2);
    CHECK_NEAR(pow10(Number(3)), 1000.0, 0.5);
    CHECK_NEAR(pow10(Number(-1)), 0.1, 1e-3);
    CHECK_NEAR(pow10(Number(-2)), 0.01, 1e-3);
    // 10^4.5 = 31623 is the documented saturation point.
    CHECK_NEAR(pow10(N(4.5)), 31623.0, 20.0);
    CHECK_NEAR(pow10(Number(20)), 31623.0, 20.0);
    CHECK(std::fabs(ToD(pow10(Number(-20)))) < 1e-4);

    // pow10(log10(x)) round-trip. The script "Power" op does NOT use this pair (it uses
    // nested square roots), so this only serves the DAS LDR chain; its error is the log10
    // error above, amplified by 10^x. Powers of two round-trip exactly.
    for (int k = 0; k <= 12; k++)
        CHECK_NEAR(pow10(log10(Number(1 << k))), (double)(1 << k), (1 << k) * 2e-3);

    double WorstRt = 0;
    for (int i = 1; i <= 200; i++)
    {
        const double x = i * 1.5;
        const double rt = ToD(pow10(log10(num(x))));
        WorstRt = std::max(WorstRt, std::fabs(rt - x) / x);
    }
    std::printf("  pow10(log10(x)) max relative error: %.5f%%\n", WorstRt * 100.0);
    CHECK(WorstRt < 0.005);
}

// ---------------------------------------------------------------------------
// Vector / Matrix (core build only)
// ---------------------------------------------------------------------------

#ifndef SCALAR_ONLY
static void TestVectors()
{
    Vector<2> Lhs;
    Lhs[0] = Number(3);
    Lhs[1] = Number(4);
    CHECK_NEAR(Lhs.Norm2(), 5.0, 1e-3);

    Vector<2> Rhs;
    Rhs[0] = Number(1);
    Rhs[1] = Number(2);
    CHECK_NEAR((Lhs + Rhs)[0], 4.0, 1e-6);
    CHECK_NEAR((Lhs - Rhs)[1], 2.0, 1e-6);
    CHECK_NEAR((Lhs * Number(2))[0], 6.0, 1e-6);

    Vector<3> Vec;
    for (int i = 0; i < 3; i++) Vec[i] = Number(i);
    const Vector<2> removed = Vec.Remove(1);
    CHECK_NEAR(removed[0], 0.0, 1e-6);
    CHECK_NEAR(removed[1], 2.0, 1e-6);

    // An out-of-range index is clamped rather than reading out of bounds.
    const Vector<2> clamped = Vec.Remove(99);
    CHECK_NEAR(clamped[0], 0.0, 1e-6);
    CHECK_NEAR(clamped[1], 1.0, 1e-6);
}

static void TestMatrix()
{
    const Matrix<3, 3> identity = Matrix<3, 3>::Identity();
    for (int Row = 0; Row < 3; Row++)
        for (int Col = 0; Col < 3; Col++)
            CHECK_NEAR(identity(Row, Col), Row == Col ? 1.0 : 0.0, 1e-6);

    // Scale and translation with no rotation must be exact.
    Vector<2> translate;
    translate[0] = Number(2);
    translate[1] = Number(3);
    const Matrix<3, 3> plain =
        Matrix<3, 3>::CreateTransform2D(Number(0), translate);
    CHECK_NEAR(plain(0, 0), 1.0, 1e-5);
    CHECK_NEAR(plain(0, 1), 0.0, 1e-4);
    CHECK_NEAR(plain(1, 0), 0.0, 1e-4);
    CHECK_NEAR(plain(1, 1), 1.0, 1e-5);
    CHECK_NEAR(plain(0, 2), 2.0, 1e-3);
    CHECK_NEAR(plain(1, 2), 3.0, 1e-3);

    // A 90-degree rotation: the shape's translation is pre-rotated, so (2, 0) must land
    // at (0, 2). This is the convention the renderer and ScriptExecTransform share.
    Vector<2> t2;
    t2[0] = Number(2);
    t2[1] = Number(0);
    const Matrix<3, 3> rotated = Matrix<3, 3>::CreateTransform2D(Number::FromRaw(RAW_HALF_PI), t2);
    CHECK_NEAR(rotated(0, 0), 0.0, 2e-3);
    CHECK_NEAR(rotated(0, 1), -1.0, 2e-3);
    CHECK_NEAR(rotated(1, 0), 1.0, 2e-3);
    CHECK_NEAR(rotated(1, 1), 0.0, 2e-3);
    CHECK_NEAR(rotated(0, 2), 0.0, 3e-3);
    CHECK_NEAR(rotated(1, 2), 2.0, 3e-3);

    // Rotating the unit X vector by 90 degrees must give the unit Y vector. The vector is
    // homogeneous (size 3) because a 3x3 transform is being applied.
    Vector<3> x;
    x[0] = Number(1);
    x[1] = Number(0);
    x[2] = Number(1);
    Vector<2> origin;
    origin[0] = Number(0);
    origin[1] = Number(0);
    const Matrix<3, 3> rot2 = Matrix<3, 3>::CreateTransform2D(Number::FromRaw(RAW_HALF_PI), origin);
    const Vector<3> turned = rot2 * x;
    CHECK_NEAR(turned[0], 0.0, 3e-3);
    CHECK_NEAR(turned[1], 1.0, 3e-3);
    CHECK_NEAR(turned[2], 1.0, 3e-3);

    // Matrix-matrix composition: Identity * M == M.
    const Matrix<3, 3> composed = identity.Multiply(rotated);
    for (int Row = 0; Row < 3; Row++)
        for (int Col = 0; Col < 3; Col++)
            CHECK_NEAR(composed(Row, Col), ToD(rotated(Row, Col)), 1e-4);
}

static void TestColour()
{
    ColourClass black(0, 0, 0, 255);
    const ColourClass white(255, 255, 255, 255);

    black.Layer(white, Number(1));
    CHECK_EQ(black.Red, 255);
    CHECK_EQ(black.Alpha, 255);

    ColourClass dark(0, 0, 0, 255);
    dark.Layer(white, Number(0));
    CHECK_EQ(dark.Red, 0);

    ColourClass mid(0, 0, 0, 255);
    mid.Layer(white, N(0.5));
    CHECK(mid.Red >= 126 && mid.Red <= 129);

    // An out-of-range overlap used to extrapolate and wrap in the uint8_t channels.
    ColourClass over(10, 20, 30, 255);
    over.Layer(white, Number(4));
    CHECK_EQ(over.Red, 255);
    CHECK_EQ(over.Green, 255);
    CHECK_EQ(over.Blue, 255);

    // A negative overlap extrapolates below zero; the clamp floors it at 0 rather than
    // letting the subtraction wrap around in the uint8_t channels.
    ColourClass under(10, 20, 30, 255);
    under.Layer(white, Number(-4));
    CHECK_EQ(under.Red, 0);
    CHECK_EQ(under.Green, 0);
    CHECK_EQ(under.Blue, 0);
}
#endif

// ---------------------------------------------------------------------------

int main()
{
    std::printf("== native numeric core tests (%s) ==\n",
#if defined(NUMBER_ONLY_32BIT) && defined(SCALAR_ONLY)
                "DAS build: 32-bit, SCALAR_ONLY"
#elif defined(NUMBER_ONLY_32BIT)
                "32-bit build: NUMBER_ONLY_32BIT, Vector/Matrix"
#else
                "core build: 64-bit"
#endif
    );

    TestBasics();
    TestFixedPrimitives();
    TestTrig();
    TestAtan2();
    TestLogExp();
#ifndef SCALAR_ONLY
    TestSqrt();
    TestVectors();
    TestMatrix();
    TestColour();
#endif

    std::printf("== %d checks, %d failures ==\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
