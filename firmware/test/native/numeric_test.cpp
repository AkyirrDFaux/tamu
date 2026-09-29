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

static int gChecks = 0;
static int gFailures = 0;

static double toD(Number value) { return value.Value / 65536.0; }

// Number(double) is deliberately ambiguous (every integer constructor needs a conversion),
// so doubles go through the N() idiom or this helper.
static Number num(double value) { return Number::FromRaw((int32_t)(value * 65536.0)); }

#define CHECK(cond)                                                                              \
    do                                                                                           \
    {                                                                                            \
        gChecks++;                                                                               \
        if (!(cond))                                                                             \
        {                                                                                        \
            gFailures++;                                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
        }                                                                                        \
    } while (0)

// Asserts that `actual` (a Number, or a raw double) is within `tol` of `expect`.
#define CHECK_NEAR(actual, expect, tol)                                                          \
    do                                                                                           \
    {                                                                                            \
        gChecks++;                                                                               \
        const double a_ = toD(actual);                                                           \
        const double e_ = (expect);                                                              \
        if (!(std::fabs(a_ - e_) <= (tol)))                                                      \
        {                                                                                        \
            gFailures++;                                                                         \
            std::printf("FAIL %s:%d: %s = %.6f, expected %.6f +/- %.6f\n", __FILE__, __LINE__,   \
                        #actual, a_, e_, (double)(tol));                                         \
        }                                                                                        \
    } while (0)

#define CHECK_EQ(actual, expect)                                                                 \
    do                                                                                           \
    {                                                                                            \
        gChecks++;                                                                               \
        if (!((actual) == (expect)))                                                             \
        {                                                                                        \
            gFailures++;                                                                         \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #actual, #expect);         \
        }                                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// Construction, arithmetic, conversion
// ---------------------------------------------------------------------------

static void testBasics()
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

static void testFixedPrimitives()
{
    const int32_t values[] = {0,       1,        -1,       2,       -2,       100,      -100,
                              65536,   12345,    -12345,   1000000, -1000000, (1 << 20), -(1 << 20),
                              46341,   -46341,   46340,    -46340,  252645135, -252645135};

    // FixedMul32 must equal the exact ((int64_t)a * b) >> 16.
    for (int32_t a : values)
    {
        for (int32_t b : values)
        {
            const int32_t expected = (int32_t)(((int64_t)a * b) >> DECIMAL);
            const int32_t got = FixedMul32(a, b);
            if (got != expected)
            {
                gFailures++;
                std::printf("FAIL FixedMul32(%d, %d) = %d, expected %d\n", a, b, got, expected);
            }
            gChecks++;
        }
    }

    // MulHigh32 must equal the high 32 bits of the exact product.
    for (int32_t a : values)
    {
        for (int32_t b : values)
        {
            const int32_t expected = (int32_t)(((int64_t)a * (int64_t)b) >> 32);
            const int32_t got = MulHigh32(a, b);
            if (got != expected)
            {
                gFailures++;
                std::printf("FAIL MulHigh32(%d, %d) = %d, expected %d\n", a, b, got, expected);
            }
            gChecks++;
        }
    }

    // FixedDiv32 must equal the exact (a << 16) / b, truncating toward zero, and 0 for b == 0.
    for (int32_t a : values)
    {
        for (int32_t b : values)
        {
            const int32_t expected = b == 0 ? 0 : (int32_t)(((int64_t)a << DECIMAL) / b);
            const int32_t got = FixedDiv32(a, b);
            if (got != expected)
            {
                gFailures++;
                std::printf("FAIL FixedDiv32(%d, %d) = %d, expected %d\n", a, b, got, expected);
            }
            gChecks++;
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
        const int32_t a = (int32_t)state;
        state = state * 1103515245u + 12345u;
        const int32_t b = (int32_t)state;

        if (MulHigh32(a, b) != (int32_t)(((int64_t)a * b) >> 32)) mismatches++;
        if (FixedMul32(a, b) != (int32_t)(((int64_t)a * b) >> DECIMAL)) mismatches++;
        // Skip quotients that cannot fit in 32 bits: FixedDiv32 is documented to be exact
        // only in that range (and the reference would overflow the cast as well).
        if (b != 0)
        {
            const int64_t q = ((int64_t)a << DECIMAL) / b;
            if (q >= INT32_MIN && q <= INT32_MAX &&
                FixedDiv32(a, b) != (int32_t)q) mismatches++;
        }
    }
    gChecks++;
    if (mismatches != 0)
    {
        gFailures++;
        std::printf("FAIL randomised fixed-point sweep: %d mismatches\n", mismatches);
    }
}

// ---------------------------------------------------------------------------
// sqrt
// ---------------------------------------------------------------------------

#ifndef SCALAR_ONLY

static void testSqrt()
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
        const double got = toD(sqrt(num(x)));
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

static void testTrig()
{
    const Number pi = GetPI();
    const Number halfPi = Number::FromRaw(RAW_HALF_PI);

    CHECK_NEAR(pi, 3.14159265, 1e-4);
    CHECK_NEAR(sin(Number(0)), 0.0, 1e-5);
    CHECK_NEAR(sin(pi), 0.0, 2e-3);
    CHECK_NEAR(sin(halfPi), 1.0, 2e-3);
    CHECK_NEAR(sin(-halfPi), -1.0, 2e-3);
    CHECK_NEAR(cos(Number(0)), 1.0, 2e-3);
    CHECK_NEAR(cos(pi), -1.0, 2e-3);
    CHECK_NEAR(cos(halfPi), 0.0, 2e-3);

    // The 45-degree cell of the rotation matrix is what the improved parabola fixed.
    // The plain Bx + Cx|x| was ~5.6 % high here; guard it explicitly.
    CHECK_NEAR(sin(Number::FromRaw(RAW_PI / 4)), 0.70710678, 0.02);
    CHECK_NEAR(cos(Number::FromRaw(RAW_PI / 4)), 0.70710678, 0.02);

    // Odd/even symmetry. The tolerance is a few Q16.16 LSBs, not zero: fixed-point multiply
    // floors (arithmetic shift), so f(-x) is not the exact negation of f(x) when a product has
    // a fractional part. This is inherent to the fixed-point representation, not a defect.
    for (int deg = -170; deg <= 170; deg += 10)
    {
        const Number a = Number::FromRaw((int32_t)((double)deg * (RAW_PI / 180.0)));
        CHECK_NEAR(sin(-a), -toD(sin(a)), 1e-4);
        CHECK_NEAR(cos(-a), toD(cos(a)), 1e-4);
    }

    // Accuracy across a full turn, and periodicity.
    double worstSin = 0;
    double worstCos = 0;
    const int steps = 3600;
    for (int i = 0; i <= steps; i++)
    {
        const double rad = (i * 2.0 * 3.14159265358979) / steps;
        const Number a = num(rad);
        worstSin = std::max(worstSin, std::fabs(toD(sin(a)) - std::sin(rad)));
        worstCos = std::max(worstCos, std::fabs(toD(cos(a)) - std::cos(rad)));
    }
    std::printf("  sin max absolute error: %.5f\n", worstSin);
    std::printf("  cos max absolute error: %.5f\n", worstCos);
    CHECK(worstSin < 0.005);
    CHECK(worstCos < 0.005);

    // sin(x + 2*PI) == sin(x).
    for (int deg = -180; deg <= 180; deg += 30)
    {
        const Number a = Number::FromRaw((int32_t)((double)deg * (RAW_PI / 180.0)));
        CHECK_NEAR(sin(a + Number::FromRaw(RAW_TWO_PI)), toD(sin(a)), 1e-3);
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
    double maxAmp = 0;
    for (int i = 0; i <= 360; i++)
    {
        const Number a = Number::FromRaw((int32_t)((double)i * (RAW_PI / 180.0)));
        maxAmp = std::max(maxAmp, std::fabs(toD(sin(a))));
    }
    std::printf("  sin peak amplitude: %.5f\n", maxAmp);
    CHECK(std::fabs(maxAmp - 1.0) < 0.005);
}

static void testAtan2()
{
    const Number pi = GetPI();

    // Exact quadrant boundaries (X = 0 is the tricky one, it must not divide by zero).
    CHECK_NEAR(atan2(Number(0), Number(1)), 0.0, 1e-4);
    CHECK_NEAR(atan2(Number(1), Number(0)), toD(pi) / 2, 1e-4);
    CHECK_NEAR(atan2(Number(-1), Number(0)), -toD(pi) / 2, 1e-4);
    CHECK_NEAR(atan2(Number(0), Number(-1)), toD(pi), 1e-4); // +PI, as std::atan2
    CHECK_NEAR(atan2(Number(0), Number(0)), 0.0, 1e-6);

    CHECK_NEAR(atan2(Number(1), Number(1)), toD(pi) / 4, 1e-3);
    CHECK_NEAR(atan2(Number(-1), Number(1)), -toD(pi) / 4, 1e-3);
    CHECK_NEAR(atan2(Number(1), Number(-1)), 3 * toD(pi) / 4, 1e-3);
    CHECK_NEAR(atan2(Number(-1), Number(-1)), -3 * toD(pi) / 4, 1e-3);

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
            double diff = toD(atan2(num(y), num(x))) - std::atan2(y, x);
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

static void testLogExp()
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
        worst = std::max(worst, std::fabs(toD(log(num(x))) - std::log(x)));
    }
    std::printf("  log max absolute error: %.5f\n", worst);
    CHECK(worst < 0.005);

    // Monotonicity holds despite the truncation, and the LDR lux chain depends on it.
    Number prevLog = log(num(0.01));
    for (int i = 1; i <= 2000; i++)
    {
        const Number cur = log(num(i * 0.5));
        CHECK(cur >= prevLog);
        prevLog = cur;
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
    CHECK(std::fabs(toD(pow10(Number(-20)))) < 1e-4);

    // pow10(log10(x)) round-trip. The script "Power" op does NOT use this pair (it uses
    // nested square roots), so this only serves the DAS LDR chain; its error is the log10
    // error above, amplified by 10^x. Powers of two round-trip exactly.
    for (int k = 0; k <= 12; k++)
        CHECK_NEAR(pow10(log10(Number(1 << k))), (double)(1 << k), (1 << k) * 2e-3);

    double worstRt = 0;
    for (int i = 1; i <= 200; i++)
    {
        const double x = i * 1.5;
        const double rt = toD(pow10(log10(num(x))));
        worstRt = std::max(worstRt, std::fabs(rt - x) / x);
    }
    std::printf("  pow10(log10(x)) max relative error: %.5f%%\n", worstRt * 100.0);
    CHECK(worstRt < 0.005);
}

// ---------------------------------------------------------------------------
// Vector / Matrix (core build only)
// ---------------------------------------------------------------------------

#ifndef SCALAR_ONLY
static void testVectors()
{
    Vector<2> a;
    a[0] = Number(3);
    a[1] = Number(4);
    CHECK_NEAR(a.norm2(), 5.0, 1e-3);

    Vector<2> b;
    b[0] = Number(1);
    b[1] = Number(2);
    CHECK_NEAR((a + b)[0], 4.0, 1e-6);
    CHECK_NEAR((a - b)[1], 2.0, 1e-6);
    CHECK_NEAR((a * Number(2))[0], 6.0, 1e-6);

    Vector<3> c;
    for (int i = 0; i < 3; i++) c[i] = Number(i);
    const Vector<2> removed = c.remove(1);
    CHECK_NEAR(removed[0], 0.0, 1e-6);
    CHECK_NEAR(removed[1], 2.0, 1e-6);

    // An out-of-range index is clamped rather than reading out of bounds.
    const Vector<2> clamped = c.remove(99);
    CHECK_NEAR(clamped[0], 0.0, 1e-6);
    CHECK_NEAR(clamped[1], 1.0, 1e-6);
}

static void testMatrix()
{
    const Matrix<3, 3> identity = Matrix<3, 3>::Identity();
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            CHECK_NEAR(identity(r, c), r == c ? 1.0 : 0.0, 1e-6);

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
    const Matrix<3, 3> composed = identity.multiply(rotated);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            CHECK_NEAR(composed(r, c), toD(rotated(r, c)), 1e-4);
}

static void testColour()
{
    ColourClass black(0, 0, 0, 255);
    const ColourClass white(255, 255, 255, 255);

    black.Layer(white, Number(1));
    CHECK_EQ(black.R, 255);
    CHECK_EQ(black.A, 255);

    ColourClass dark(0, 0, 0, 255);
    dark.Layer(white, Number(0));
    CHECK_EQ(dark.R, 0);

    ColourClass mid(0, 0, 0, 255);
    mid.Layer(white, N(0.5));
    CHECK(mid.R >= 126 && mid.R <= 129);

    // An out-of-range overlap used to extrapolate and wrap in the uint8_t channels.
    ColourClass over(10, 20, 30, 255);
    over.Layer(white, Number(4));
    CHECK_EQ(over.R, 255);
    CHECK_EQ(over.G, 255);
    CHECK_EQ(over.B, 255);

    // A negative overlap extrapolates below zero; the clamp floors it at 0 rather than
    // letting the subtraction wrap around in the uint8_t channels.
    ColourClass under(10, 20, 30, 255);
    under.Layer(white, Number(-4));
    CHECK_EQ(under.R, 0);
    CHECK_EQ(under.G, 0);
    CHECK_EQ(under.B, 0);
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

    testBasics();
    testFixedPrimitives();
    testTrig();
    testAtan2();
    testLogExp();
#ifndef SCALAR_ONLY
    testSqrt();
    testVectors();
    testMatrix();
    testColour();
#endif

    std::printf("== %d checks, %d failures ==\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
