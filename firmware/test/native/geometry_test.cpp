// Host-side test for the LED display's geometry math (Blocks/GeometryMath.h).
//
// The shapes are only ever checked by eye on the rig, and the atan2 fix already changed the
// Polygon/Star sector lookup once. These are *geometric properties* rather than golden values:
// inside/outside, the mirror and rotation axes each shape actually has, monotonicity of the
// fade, determinism of the noise, and the range of every shape over a parameter sweep. A sign
// flip, a swapped axis or a broken sector - the kind of thing the numeric fixes produced -
// fails here. It also caught a real bug: the rounded-rectangle signed distance pulled the
// straight sides in by the corner radius (see the Rounding case in GeometryMath.h).
//
// Pure Number/Vector math: build with -I firmware/src and a plain host compiler.

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <initializer_list>

#include "Blocks/GeometryMath.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__,    \
                        #got, g, w);                                                 \
        }                                                                            \
    } while (0)

// Number has no double constructor by design (fixed point is the only representation), so the
// test states its intent as "value * 65536" in one place.
static Number num(double value) { return Number::FromRaw((int32_t)(value * 65536.0)); }
static double toD(Number value) { return value.Value / 65536.0; }

static const double PI = 3.14159265358979;

static GeometryParams Make(Geometries shape, double sizeX, double sizeY, double fade,
                           double angles = 0.0, uint8_t points = 0, double rounding = 0.0)
{
    GeometryParams p;
    p.Shape = shape;
    p.SizeX = num(sizeX);
    p.SizeY = num(sizeY);
    p.Fade = num(fade);
    p.Rounding = num(rounding);
    p.Angles = num(angles);
    p.PointNumber = points;
    p.Alpha = N(1);
    PrepareGeometry(p);
    return p;
}

static Vector<2> At(double x, double y) { return Vector<2>{num(x), num(y)}; }

// Reflects P across the line through the origin at angle `phi` (phi = 0 is the x axis).
static Vector<2> Mirror(double x, double y, double phi)
{
    const double c = std::cos(2 * phi), s = std::sin(2 * phi);
    return At(c * x + s * y, s * x - c * y);
}

// Rotates P by `phi` about the origin.
static Vector<2> Rotate(double x, double y, double phi)
{
    const double c = std::cos(phi), s = std::sin(phi);
    return At(c * x - s * y, s * x + c * y);
}

// Mirror images are compared with a small tolerance: the two sides take different
// fixed-point paths, so they can differ by ~1 LSB - and with a hard edge a 1-LSB difference
// exactly on the boundary flips 0 <-> 255. A wrong axis makes thousands of points differ by a
// wide margin (the rounded-rectangle bug gave 255 vs 0 over a whole side), so this stays sharp.
// The shapes under test carry a small fade for the same reason.
static const int AlphaTol = 2;

static bool Close(uint8_t a, uint8_t b)
{
    const int d = (int)a - (int)b;
    return d <= AlphaTol && d >= -AlphaTol;
}

// Near a shape's edge the alpha is genuinely discontinuous: a point exactly on the boundary
// can land either side of it, and a fixed-point difference of one step picks that side (the
// star's sector steps do this at every boundary line). Points in the fade band are therefore
// skipped - the comparison still catches any real asymmetry, which shows up as a *definite*
// inside vs *definite* outside.
static bool OnEdge(uint8_t a) { return a > 4 && a < 251; }

// Asserts ShapeAlpha is a mirror symmetry about `phi` over a grid, and reports each failure
// once per axis rather than per point (the sweep is deliberately dense).
static void CheckMirror(const GeometryParams &p, const char *label, double phi)
{
    int bad = 0, compared = 0;
    for (double x = -8; x <= 8; x += 0.5)
        for (double y = -8; y <= 8; y += 0.5)
        {
            const uint8_t a = ShapeAlpha(p, At(x, y));
            const uint8_t m = ShapeAlpha(p, Mirror(x, y, phi));
            if (OnEdge(a) || OnEdge(m)) continue;
            compared++;
            if (!Close(a, m))
            {
                if (bad == 0)
                    std::printf("  [%s] mirror %.1f deg: (%g,%g) %u vs %u\n", label,
                                phi * 180 / PI, x, y, (unsigned)a, (unsigned)m);
                bad++;
            }
        }
    checks++;
    if (compared < 100)
    {
        failures++;
        std::printf("FAIL mirror symmetry [%s] %.1f deg: only %d points compared\n",
                    label, phi * 180 / PI, compared);
    }
    if (bad)
    {
        failures++;
        std::printf("FAIL mirror symmetry [%s] %.1f deg: %d points differ\n", label,
                    phi * 180 / PI, bad);
    }
}

// Asserts the shape is invariant under a rotation by `phi`.
static void CheckRotate(const GeometryParams &p, const char *label, double phi)
{
    int bad = 0, compared = 0;
    for (double x = -8; x <= 8; x += 0.5)
        for (double y = -8; y <= 8; y += 0.5)
        {
            const uint8_t a = ShapeAlpha(p, At(x, y));
            const uint8_t r = ShapeAlpha(p, Rotate(x, y, phi));
            if (OnEdge(a) || OnEdge(r)) continue;
            compared++;
            if (!Close(a, r))
            {
                if (bad == 0)
                    std::printf("  [%s] rotate %.1f deg: (%g,%g) %u vs %u\n", label,
                                phi * 180 / PI, x, y, (unsigned)a, (unsigned)r);
                bad++;
            }
        }
    checks++;
    if (compared < 100)
    {
        failures++;
        std::printf("FAIL rotation symmetry [%s] %.1f deg: only %d points compared\n",
                    label, phi * 180 / PI, compared);
    }
    if (bad)
    {
        failures++;
        std::printf("FAIL rotation symmetry [%s] %.1f deg: %d points differ\n", label,
                    phi * 180 / PI, bad);
    }
}

// Polygon/Star put a radial *step* on their sector boundaries (the outer and inner wedges
// meet there), so a grid sample lands exactly on a boundary ray and the alpha there depends on
// which side a rounding picks - not on whether the sector math is right. These sample polar
// points offset from every boundary instead, where the shape is well defined.
static void CheckSymmetryPolar(const GeometryParams &p, const char *label, double sector,
                               double phi, bool rotate)
{
    int bad = 0, compared = 0;
    for (int k = 0; k < 40; k++)
    {
        const double theta = (k + 0.37) * sector / 4.0;
        for (double r : {0.5, 1.5, 3.0, 4.5, 6.0, 7.0})
        {
            const double x = r * std::cos(theta), y = r * std::sin(theta);
            const uint8_t a = ShapeAlpha(p, At(x, y));
            const Vector<2> q = rotate ? Rotate(x, y, phi) : Mirror(x, y, phi);
            const uint8_t b = ShapeAlpha(p, q);
            compared++;
            if (OnEdge(a) || OnEdge(b)) continue;
            if (!Close(a, b))
            {
                if (bad == 0)
                    std::printf("  [%s] %s %.1f deg: (%g,%g) %u vs %u\n", label,
                                rotate ? "rotate" : "mirror", phi * 180 / PI, x, y,
                                (unsigned)a, (unsigned)b);
                bad++;
            }
        }
    }
    checks++;
    if (compared < 100)
    {
        failures++;
        std::printf("FAIL %s symmetry [%s] %.1f deg: only %d points compared\n",
                    rotate ? "rotation" : "mirror", label, phi * 180 / PI, compared);
    }
    if (bad)
    {
        failures++;
        std::printf("FAIL %s symmetry [%s] %.1f deg: %d points differ\n",
                    rotate ? "rotation" : "mirror", label, phi * 180 / PI, bad);
    }
}

int main()
{
    // --- Fill / HalfFill / None ------------------------------------------------
    {
        GeometryParams fill = Make(Geometries::Fill, 4, 4, 0);
        CHECK_EQ(ShapeAlpha(fill, At(0, 0)), 255);
        CHECK_EQ(ShapeAlpha(fill, At(100, -100)), 255);

        GeometryParams half = Make(Geometries::HalfFill, 4, 4, 0.5);
        CHECK_EQ(ShapeAlpha(half, At(0, 1)), 255); // y > 0 filled
        CHECK_EQ(ShapeAlpha(half, At(0, -1)), 0);  // y < 0 empty
        CHECK_EQ(ShapeAlpha(half, At(50, 0.5)), 255);
        CheckMirror(half, "HalfFill", PI / 2); // symmetric under x -> -x

        GeometryParams none;
        none.Shape = Geometries::None;
        CHECK_EQ(ShapeAlpha(none, At(0, 0)), 0);
    }

    // --- Circle: Size is the diameter, so the radius is Size/2 ------------------
    {
        GeometryParams c = Make(Geometries::Circle, 10, 10, 0);
        CHECK(c.HalfX.Value == num(5).Value);
        CHECK_EQ(ShapeAlpha(c, At(0, 0)), 255);
        CHECK_EQ(ShapeAlpha(c, At(3, 4)), 255);   // r = 5, exactly on the radius
        CHECK_EQ(ShapeAlpha(c, At(4.5, 0)), 255); // inside
        CHECK_EQ(ShapeAlpha(c, At(5.5, 0)), 0);   // outside
        CHECK_EQ(ShapeAlpha(c, At(0, -30)), 0);
        CheckMirror(c, "Circle", 0.0);
        CheckMirror(c, "Circle", PI / 2);
        CheckMirror(c, "Circle", PI / 4); // a circle is symmetric about every axis
        CheckRotate(c, "Circle", PI / 3);
    }

    // --- The fade is monotone and bounded --------------------------------------
    {
        GeometryParams c = Make(Geometries::Circle, 10, 10, 4); // 4 px soft edge
        int prev = 256;
        for (int i = 0; i <= 60; i++)
        {
            const uint8_t a = ShapeAlpha(c, At(i * 0.25, 0));
            CHECK(a <= prev); // non-increasing with radius
            prev = a;
        }
        CHECK_EQ(ShapeAlpha(c, At(0, 0)), 255);
        CHECK_EQ(ShapeAlpha(c, At(20, 0)), 0);
    }

    // --- Square / Rectangle: extents, both axes, and both diagonals -------------
    for (const auto shape : {Geometries::Square, Geometries::Rectangle})
    {
        GeometryParams p = Make(shape, 8, 6, 0.5);
        CHECK_EQ(ShapeAlpha(p, At(0, 0)), 255);
        CHECK_EQ(ShapeAlpha(p, At(3.5, 2.5)), 255); // inside HalfX = 4, HalfY = 3
        CHECK_EQ(ShapeAlpha(p, At(4.5, 0)), 0);     // outside HalfX
        CHECK_EQ(ShapeAlpha(p, At(0, 3.5)), 0);     // outside HalfY
        CheckMirror(p, "Square/Rect", 0.0);
        CheckMirror(p, "Square/Rect", PI / 2);
        CheckRotate(p, "Square/Rect", PI);
    }
    {
        // A square is also symmetric about its diagonals (a rectangle is not).
        GeometryParams sq = Make(Geometries::Square, 8, 8, 0.5);
        CheckMirror(sq, "Square", PI / 4);
        CheckRotate(sq, "Square", PI / 2);
    }

    // --- Circle / Ellipse: a circle has every axis, an ellipse its two ----------
    {
        GeometryParams c = Make(Geometries::Circle, 12, 12, 1);
        GeometryParams e = Make(Geometries::Ellipse, 12, 8, 1);
        CheckMirror(c, "Circle(fade)", PI / 6);
        CheckMirror(e, "Ellipse", 0.0);
        CheckMirror(e, "Ellipse", PI / 2);
        CheckRotate(e, "Ellipse", PI);
        // An ellipse with equal semi-axes is a circle (up to the fixed-point path).
        GeometryParams e2 = Make(Geometries::Ellipse, 10, 10, 0);
        GeometryParams c2 = Make(Geometries::Circle, 10, 10, 0);
        int worst = 0;
        for (double x = -6; x <= 6; x += 1.0)
            for (double y = -6; y <= 6; y += 1.0)
            {
                const int d = (int)ShapeAlpha(e2, At(x, y)) - (int)ShapeAlpha(c2, At(x, y));
                if (d < 0 ? -d > worst : d > worst) worst = d < 0 ? -d : d;
            }
        CHECK(worst <= 2);
    }

    // --- Triangle / Trapezoid / DoubleParabola: symmetric under x -> -x only ---
    {
        GeometryParams t = Make(Geometries::Triangle, 8, 8, 1.5);
        GeometryParams tr = Make(Geometries::Trapezoid, 10, 6, 1.5, 30);
        GeometryParams dp = Make(Geometries::DoubleParabola, 8, 6, 1.5);
        for (auto *p : {&t, &tr, &dp})
        {
            CheckMirror(*p, "upward shape", PI / 2);
            CheckRotate(*p, "upward shape", 0.0); // trivial: rotation by 0
        }
        CHECK_EQ(ShapeAlpha(t, At(0, 0)), 255);
        CHECK_EQ(ShapeAlpha(t, At(0, 6)), 0);  // above the apex
        CHECK_EQ(ShapeAlpha(t, At(100, 0)), 0);
        CHECK_EQ(ShapeAlpha(tr, At(0, 0)), 255);
        CHECK(tr.TrapTopHalf.Value < tr.HalfX.Value); // top narrower than the bottom
        CHECK_EQ(ShapeAlpha(tr, At(20, 0)), 0);
    }

    // --- Rounded rectangle: rounding touches the corners, not the sides --------
    {
        GeometryParams sharp = Make(Geometries::Rectangle, 10, 6, 0, 0, 0, 0);
        GeometryParams round = Make(Geometries::Rectangle, 10, 6, 0, 0, 0, 2);
        CHECK_EQ(ShapeAlpha(round, At(0, 0)), 255);   // centre still opaque
        CHECK_EQ(ShapeAlpha(round, At(0, 2.5)), 255); // the flat sides are unaffected
        CHECK_EQ(ShapeAlpha(round, At(0, 2.9)), 255); // right up to the edge
        CHECK_EQ(ShapeAlpha(round, At(0, 3.1)), 0);   // and not past it
        CHECK_EQ(ShapeAlpha(round, At(4.5, 0)), 255); // same for the long side
        CHECK_EQ(ShapeAlpha(round, At(4.9, 0)), 255);
        CHECK_EQ(ShapeAlpha(round, At(5.1, 0)), 0);
        CHECK_EQ(ShapeAlpha(sharp, At(4.6, 2.6)), 255); // a sharp corner is filled
        CHECK_EQ(ShapeAlpha(round, At(4.6, 2.6)), 0);   // the rounded one is not
        CheckMirror(round, "Rounded rect", 0.0);
        CheckMirror(round, "Rounded rect", PI / 2);
    }

    // --- Polygon: a mirror axis through a vertex and through a sector bisector --
    // (Sector placement: the sector boundaries are at k*sector, bisectors at
    // halfSector + k*sector - the shape repeats every sector, so both are axes.)
    {
        GeometryParams poly = Make(Geometries::Polygon, 8, 8, 0.5, 0, 5);
        CHECK_EQ(ShapeAlpha(poly, At(0, 0)), 255);
        CHECK_EQ(ShapeAlpha(poly, At(8.5, 0)), 0); // beyond the radius
        const double sector = toD(poly.PolySector), half = toD(poly.PolyHalfSector);
        CHECK(std::fabs(half - sector / 2) < 1e-6);
        CheckSymmetryPolar(poly, "Polygon(5)", sector, 0.0, false);
        CheckSymmetryPolar(poly, "Polygon(5)", sector, half, false);
        CheckSymmetryPolar(poly, "Polygon(5)", sector, 2 * half, false);
        CheckSymmetryPolar(poly, "Polygon(5)", sector, sector, true);
        CheckSymmetryPolar(poly, "Polygon(5)", sector, 2 * sector, true);
        // An even-sided polygon is also symmetric about the y axis (90 deg).
        GeometryParams poly6 = Make(Geometries::Polygon, 8, 8, 0.5, 0, 6);
        CheckSymmetryPolar(poly6, "Polygon(6)", toD(poly6.PolySector), PI / 2, false);
    }

    // --- Star: 2n sectors alternating outer/inner, inner = Angles * R ----------
    {
        GeometryParams star = Make(Geometries::Star, 8, 8, 0.5, 0.5, 5);
        CHECK(toD(star.StarInnerR) == 4.0);
        CHECK(std::fabs(toD(star.PolySector) * 2 * 5 - 2 * PI) < 0.01); // 2n sectors
        CHECK_EQ(ShapeAlpha(star, At(0, 0)), 255);
        const double half = toD(star.PolyHalfSector);
        // The outer wedges are centred on the bisectors, so a point inside the outer radius
        // but outside the inner one is cut at the inner bisectors.
        const double outer = 0.6 * 8.0, r = outer;
        CHECK(ShapeAlpha(star, Rotate(r, 0, half)) > 224);        // outer wedge bisector
        CHECK(ShapeAlpha(star, Rotate(r, 0, 3 * half)) < 32);     // inner wedge bisector
        CHECK(ShapeAlpha(star, Rotate(0.4 * 8.0, 0, 3 * half)) > 224); // inside the inner
        CheckSymmetryPolar(star, "Star", toD(star.PolySector), half, false);
        CheckSymmetryPolar(star, "Star", toD(star.PolySector), 2 * PI / 5, true);
    }

    // --- Noise: deterministic and in range ------------------------------------
    {
        GeometryParams n1 = Make(Geometries::Noise, 4, 4, 0);
        n1.NoiseSeed = 12345;
        GeometryParams n2 = n1;
        for (double x = -5; x <= 5; x += 0.9)
            for (double y = -5; y <= 5; y += 0.9)
            {
                const uint8_t a = ShapeAlpha(n1, At(x, y));
                CHECK_EQ(ShapeAlpha(n2, At(x, y)), a);
                CHECK_EQ(ShapeAlpha(n1, At(x, y)), a);
            }
    }

    // --- Every shape stays in range over a parameter sweep --------------------
    for (const auto shape : {Geometries::None, Geometries::Fill, Geometries::HalfFill,
                             Geometries::Square, Geometries::Rectangle, Geometries::Circle,
                             Geometries::Ellipse, Geometries::DoubleParabola,
                             Geometries::Triangle, Geometries::Trapezoid,
                             Geometries::Polygon, Geometries::Star, Geometries::Noise})
    {
        for (double size : {0.5, 1.0, 5.0, 40.0})
        {
            for (double fade : {0.0, 0.5, 3.0})
            {
                for (uint8_t points : {(uint8_t)0, (uint8_t)3, (uint8_t)7, (uint8_t)32})
                {
                    for (double angles : {0.0, 0.25, 45.0, 160.0})
                    {
                        GeometryParams p = Make(shape, size, size * 0.75, fade, angles, points, 1.5);
                        p.NoiseSeed = (uint32_t)(size * 1000) ^ points;
                        for (double x = -3 * size; x <= 3 * size; x += size)
                            for (double y = -3 * size; y <= 3 * size; y += size)
                            {
                                const uint8_t a = ShapeAlpha(p, At(x, y));
                                CHECK(a <= 255); // the return type; asserts no wrap
                                (void)a;
                            }
                    }
                }
            }
        }
    }

    std::printf("== geometry tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
