#pragma once

// Geometry shape math for the LED display's mask pass (Docs/Modules and blocks/LED display.md).
//
// Pure Number/Vector math over GeometryParams: no device dependency at all, which is why it
// lives outside the render pipeline - the shapes can be exercised on the host
// (test/native/geometry_test.cpp) and the cost of the per-LED pass can be measured, instead of
// being verifiable by eye only. The pipeline keeps the caching and dispatch; this keeps the
// shape definitions.

#include "Core/Types/Number.h"
#include "Core/Types/Vector.h"
#include "Blocks/Render.h"

// Per-geometry parameters resolved once per field (cached mask recompute), so the
// per-LED shape math reads a fixed struct instead of re-walking the dictionary.
struct GeometryParams
{
    Geometries Shape = Geometries::None;
    Number SizeX = N(0), SizeY = N(0); // extent (Number = side/diameter/radius per shape)
    Number Rounding = N(0);            // px (rounded Square/Rectangle corners)
    Number Angles = N(0);              // trapezoid slant / isosceles apex (deg), star inner ratio
    Number Fade = N(1);                // px soft edge
    Number Alpha = N(1);               // 0..1 mask scale
    uint8_t PointNumber = 0;
    uint32_t NoiseSeed = 0;

    // Loop invariants of the per-LED shape pass, filled by PrepareGeometry() once per geometry.
    // Every one of these was a division or a trig call *inside* the per-LED loop, and a 64-bit
    // fixed-point divide per LED was the most expensive operation in the geometry pass.
    Number HalfX = N(0), HalfY = N(0);       // SizeX/2, SizeY/2
    Number InvHalfX = N(0), InvHalfY = N(0); // 1/HalfX, 1/HalfY
    Number InvFade = N(0);                   // 1/Fade (0 when Fade <= 0: a hard edge)
    Number InvSizeX = N(0), InvSizeY = N(0); // 1/SizeX, 1/SizeY
    Number RectInnerX = N(0), RectInnerY = N(0); // Half - Rounding
    Number EllipseMinHalf = N(0);            // min(HalfX, HalfY)
    Number NoiseInvX = N(1), NoiseInvY = N(1); // Noise cell lookup (divisor clamped to >= 1)
    Number ParabolaK = N(0);                 // SizeX / SizeY^2 (double parabola)
    Number SlantTan = N(0);                  // tan(slant) (trapezoid)
    Number TrapTopHalf = N(0), TrapSlope = N(0); // trapezoid top half-width, (top-bottom)/2
    Number TriHalfH = N(0), TriFactor = N(0);    // triangle: h/2, 2h/w
    Number PolySector = N(0), PolyHalfSector = N(0), PolyCosHalf = N(0);
    Number InvSector = N(0), StarInnerR = N(0); // polygon/star
};

// Precomputes the per-geometry invariants above. Called once per geometry field (not per LED).
static inline void PrepareGeometry(GeometryParams &Params)
{
    const Number Two = N(2);
    Params.HalfX = Params.SizeX / Two;
    Params.HalfY = Params.SizeY / Two;
    Params.InvHalfX = (Params.HalfX.Value != 0) ? N(1) / Params.HalfX : N(0);
    Params.InvHalfY = (Params.HalfY.Value != 0) ? N(1) / Params.HalfY : N(0);
    Params.InvFade = (Params.Fade.Value > 0) ? N(1) / Params.Fade : N(0);
    Params.InvSizeX = (Params.SizeX.Value != 0) ? N(1) / Params.SizeX : N(0);
    Params.InvSizeY = (Params.SizeY.Value != 0) ? N(1) / Params.SizeY : N(0);
    Params.RectInnerX = Params.HalfX - Params.Rounding;
    Params.RectInnerY = Params.HalfY - Params.Rounding;
    Params.EllipseMinHalf = (Params.HalfX.Value < Params.HalfY.Value) ? Params.HalfX : Params.HalfY;
    // The Noise cell lookup clamps its divisor to at least 1 (a zero size is an empty shape).
    Params.NoiseInvX = (Params.SizeX.Value > 0) ? Params.InvSizeX : N(1);
    Params.NoiseInvY = (Params.SizeY.Value > 0) ? Params.InvSizeY : N(1);

    const Number SqH = sq(Params.SizeY);
    Params.ParabolaK = (SqH.Value != 0) ? Params.SizeX / SqH : N(0);

    // Trapezoid: Size = [bottom width, height], Angles = side slant (deg).
    const Number SlantRad = Params.Angles * GetPI() / N(180);
    const Number CosSlant = cos(SlantRad);
    Params.SlantTan = (CosSlant.Value != 0) ? sin(SlantRad) / CosSlant : N(0);
    Number Top = Params.SizeX - Two * Params.SizeY * Params.SlantTan;
    if (Top.Value < 0) Top = N(0);
    Params.TrapTopHalf = Top / Two;
    Params.TrapSlope = Params.TrapTopHalf - Params.HalfX;

    // Triangle: isosceles (Size = equal-side length, Angles = apex angle in degrees).
    // Angles defaults to 60 when unset; a 0-degree apex is degenerate and would collapse
    // the shape, and 60 reproduces the equilateral case as one instance of isosceles.
    const Number Apex = (Params.Angles.Value > 0) ? Params.Angles : N(60);
    const Number HalfApex = Apex * GetPI() / N(180) / Two;
    const Number TriW = Two * Params.SizeX * sin(HalfApex);
    const Number TriH = Params.SizeX * cos(HalfApex);
    Params.TriHalfH = TriH / Two;
    Params.TriFactor = (TriW.Value != 0) ? Two * TriH / TriW : N(0);

    // Polygon/Star: the sector is constant per geometry.
    int Points = Params.PointNumber < 3 ? 3 : Params.PointNumber;
    if (Points > 32) Points = 32;
    Params.PolySector = (Params.Shape == Geometries::Star) ? (Two * GetPI() / Number(Points * 2))
                                                 : (Two * GetPI() / Number(Points));
    Params.PolyHalfSector = Params.PolySector / Two;
    Params.PolyCosHalf = cos(Params.PolyHalfSector);
    Params.InvSector = (Params.PolySector.Value != 0) ? N(1) / Params.PolySector : N(0);
    const Number Inner = (Params.Angles.Value > 0) ? Params.Angles : N(0.5);
    Params.StarInnerR = Params.SizeX * Inner;
}

// Converts a signed edge distance (pixels, positive inside) into a 0..255 alpha with a
// `fade`-pixel soft edge (alpha 1 at distance >= fade/2, 0 at <= -fade/2). Fade <= 0 = hard edge.
static inline uint8_t FadeAlpha(Number Distance, const GeometryParams &Params)
{
    if (Params.Fade.Value <= 0)
        return (Distance.Value >= 0) ? 255 : 0;
    // Multiply by the reciprocal prepared once per geometry instead of dividing per LED.
    return (uint8_t)(LimitZeroToOne(Distance * Params.InvFade + N(0.5)) * N(255)).RoundToInt();
}

// Computes the 0..255 alpha of the shape in `p` at point P (shape-local space).
// Signed-distance shapes go through FadeAlpha; Fill/Noise return alpha directly.
//
// This is also the *reference* for the pipeline's per-LED loop: the loop specialises the shape
// dispatch, and the host test checks the two agree over a parameter sweep.
static inline uint8_t ShapeAlpha(const GeometryParams &Params, const Vector<2> &Point)
{
    switch (Params.Shape)
    {
    case Geometries::Fill:
        return 255;

    case Geometries::HalfFill:
        // Half-plane: fill where y >= 0 (tilt/offset via the transform).
        return FadeAlpha(Point[1], Params);

    case Geometries::Square:
    case Geometries::Rectangle:
    {
        if (Params.Rounding.Value > 0)
        {
            // Rounded-rectangle signed distance, positive inside. The standard form is
            //   d = r - length(max(q,0)) - min(max(q.x,q.y), 0)
            // with q = |P| - (Half - r): the m term corrects the interior (it is negative
            // there) and must NOT be subtracted when the point is out in the side band,
            // where q is positive. Subtracting it there pulled every *straight* side in
            // towards the inner rectangle, so a rounded Square/Rectangle lost ~Rounding px
            // from all four sides instead of only rounding the corners.
            Number Rx = abs(Point[0]) - Params.RectInnerX;
            Number Ry = abs(Point[1]) - Params.RectInnerY;
            Number Lx = Rx.Value > 0 ? Rx : N(0);
            Number Ly = Ry.Value > 0 ? Ry : N(0);
            Number CornerDist = Rx.Value > Ry.Value ? Rx : Ry;
            Number Dist = Params.Rounding - Vector<2>{Lx, Ly}.Norm2() - (CornerDist.Value < 0 ? CornerDist : N(0));
            return FadeAlpha(Dist, Params);
        }
        return FadeAlpha(min(Params.HalfX - abs(Point[0]), Params.HalfY - abs(Point[1])), Params);
    }

    case Geometries::Circle:
        return FadeAlpha(Params.HalfX - Point.Norm2(), Params);

    case Geometries::Ellipse:
    {
        Number Radial = sqrt(sq(Point[0] * Params.InvHalfX) + sq(Point[1] * Params.InvHalfY));
        return FadeAlpha((N(1) - Radial) * Params.EllipseMinHalf, Params);
    }

    case Geometries::Trapezoid:
    {
        // Isosceles trapezoid: Size = [bottom width, height], Angles = side slant (deg).
        // 0 at the bottom, 1 at the top; the half-widths and the slope are per-geometry.
        Number Frac = (Point[1] + Params.HalfY) * Params.InvSizeY;
        Number Hw = Params.HalfX + Params.TrapSlope * Frac;
        Number Dist = min(Params.HalfY - abs(Point[1]), Hw - abs(Point[0]));
        return FadeAlpha(Dist, Params);
    }

    case Geometries::DoubleParabola:
    {
        return FadeAlpha(-(abs(Point[0]) - Params.SizeX + sq(Point[1]) * Params.ParabolaK), Params);
    }

    case Geometries::Triangle:
    {
        // Isosceles (Size = equal side, Angles = apex angle in deg, default 60). Centered:
        // apex up, base horizontal.
        Number Dist = min(Params.TriHalfH - Point[1] - Params.TriFactor * abs(Point[0]), Point[1] + Params.TriHalfH);
        return FadeAlpha(Dist, Params);
    }

    case Geometries::Polygon:
    case Geometries::Star:
    {
        Number OuterR = Params.SizeX;
        if (OuterR.Value <= 0) return 0;
        int Points = Params.PointNumber < 3 ? 3 : Params.PointNumber;
        if (Points > 32) Points = 32;
        Number PointR = Point.Norm2();
        if (PointR.Value <= 0) return FadeAlpha(OuterR, Params);
        Number Theta = atan2(Point[1], Point[0]);
        if (Theta.Value < 0) Theta = Theta + 2 * GetPI();

        const Number Sector = Params.PolySector;
        Number Radius = OuterR;
        if (Params.Shape == Geometries::Star)
        {
            // Star: 2n vertices alternating outer R / inner R*ratio (Angles or 0.5).
            uint16_t Vi = (uint16_t)(Theta * Params.InvSector).ToInt();
            Radius = (Vi & 1) ? Params.StarInnerR : OuterR;
        }
        Number Ang = (Theta - Number((Theta * Params.InvSector).ToInt()) * Sector) - Params.PolyHalfSector;
        // The per-pixel cos(ang) keeps a divide (it cannot be hoisted); everything else is
        // precomputed per geometry.
        Number Dist = Radius * Params.PolyCosHalf / cos(Ang) - PointR;
        return FadeAlpha(Dist, Params);
    }

    case Geometries::Noise:
    {
        // Deterministic per-cell hash of (NoiseSeed, scaled coords) -> 0..255.
        int32_t x = (Point[0] * Params.NoiseInvX).ToInt();
        int32_t y = (Point[1] * Params.NoiseInvY).ToInt();
        uint32_t Hash = Params.NoiseSeed;
        Hash ^= (uint32_t)(x * 2654435761);
        Hash *= 16777619;
        Hash ^= (uint32_t)(y * 1597334677);
        Hash *= 16777619;
        Hash ^= Hash >> 13;
        return (uint8_t)((Hash >> 8) & 0xFF);
    }

    default:
        return 0;
    }
}
