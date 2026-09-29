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
static inline void PrepareGeometry(GeometryParams &p)
{
    const Number two = N(2);
    p.HalfX = p.SizeX / two;
    p.HalfY = p.SizeY / two;
    p.InvHalfX = (p.HalfX.Value != 0) ? N(1) / p.HalfX : N(0);
    p.InvHalfY = (p.HalfY.Value != 0) ? N(1) / p.HalfY : N(0);
    p.InvFade = (p.Fade.Value > 0) ? N(1) / p.Fade : N(0);
    p.InvSizeX = (p.SizeX.Value != 0) ? N(1) / p.SizeX : N(0);
    p.InvSizeY = (p.SizeY.Value != 0) ? N(1) / p.SizeY : N(0);
    p.RectInnerX = p.HalfX - p.Rounding;
    p.RectInnerY = p.HalfY - p.Rounding;
    p.EllipseMinHalf = (p.HalfX.Value < p.HalfY.Value) ? p.HalfX : p.HalfY;
    // The Noise cell lookup clamps its divisor to at least 1 (a zero size is an empty shape).
    p.NoiseInvX = (p.SizeX.Value > 0) ? p.InvSizeX : N(1);
    p.NoiseInvY = (p.SizeY.Value > 0) ? p.InvSizeY : N(1);

    const Number sqH = sq(p.SizeY);
    p.ParabolaK = (sqH.Value != 0) ? p.SizeX / sqH : N(0);

    // Trapezoid: Size = [bottom width, height], Angles = side slant (deg).
    const Number slantRad = p.Angles * GetPI() / N(180);
    const Number cosSlant = cos(slantRad);
    p.SlantTan = (cosSlant.Value != 0) ? sin(slantRad) / cosSlant : N(0);
    Number top = p.SizeX - two * p.SizeY * p.SlantTan;
    if (top.Value < 0) top = N(0);
    p.TrapTopHalf = top / two;
    p.TrapSlope = p.TrapTopHalf - p.HalfX;

    // Triangle: equilateral (Size = side) by default, isosceles when Angles is set.
    Number triW, triH;
    if (p.Angles.Value > 0)
    {
        const Number halfApex = p.Angles * GetPI() / N(180) / two;
        triW = two * p.SizeX * sin(halfApex);
        triH = p.SizeX * cos(halfApex);
    }
    else
    {
        triW = p.SizeX;
        triH = p.SizeX * sqrt(N(3)) / two;
    }
    p.TriHalfH = triH / two;
    p.TriFactor = (triW.Value != 0) ? two * triH / triW : N(0);

    // Polygon/Star: the sector is constant per geometry.
    int n = p.PointNumber < 3 ? 3 : p.PointNumber;
    if (n > 32) n = 32;
    p.PolySector = (p.Shape == Geometries::Star) ? (two * GetPI() / Number(n * 2))
                                                 : (two * GetPI() / Number(n));
    p.PolyHalfSector = p.PolySector / two;
    p.PolyCosHalf = cos(p.PolyHalfSector);
    p.InvSector = (p.PolySector.Value != 0) ? N(1) / p.PolySector : N(0);
    const Number inner = (p.Angles.Value > 0) ? p.Angles : N(0.5);
    p.StarInnerR = p.SizeX * inner;
}

// Converts a signed edge distance (pixels, positive inside) into a 0..255 alpha with a
// `fade`-pixel soft edge (alpha 1 at distance >= fade/2, 0 at <= -fade/2). Fade <= 0 = hard edge.
static inline uint8_t FadeAlpha(Number distance, const GeometryParams &p)
{
    if (p.Fade.Value <= 0)
        return (distance.Value >= 0) ? 255 : 0;
    // Multiply by the reciprocal prepared once per geometry instead of dividing per LED.
    return (uint8_t)(LimitZeroToOne(distance * p.InvFade + N(0.5)) * N(255)).RoundToInt();
}

// Computes the 0..255 alpha of the shape in `p` at point P (shape-local space).
// Signed-distance shapes go through FadeAlpha; Fill/Noise return alpha directly.
//
// This is also the *reference* for the pipeline's per-LED loop: the loop specialises the shape
// dispatch, and the host test checks the two agree over a parameter sweep.
static inline uint8_t ShapeAlpha(const GeometryParams &p, const Vector<2> &P)
{
    switch (p.Shape)
    {
    case Geometries::Fill:
        return 255;

    case Geometries::HalfFill:
        // Half-plane: fill where y >= 0 (tilt/offset via the transform).
        return FadeAlpha(P[1], p);

    case Geometries::Square:
    case Geometries::Rectangle:
    {
        if (p.Rounding.Value > 0)
        {
            // Rounded-rectangle signed distance, positive inside. The standard form is
            //   d = r - length(max(q,0)) - min(max(q.x,q.y), 0)
            // with q = |P| - (Half - r): the m term corrects the interior (it is negative
            // there) and must NOT be subtracted when the point is out in the side band,
            // where q is positive. Subtracting it there pulled every *straight* side in
            // towards the inner rectangle, so a rounded Square/Rectangle lost ~Rounding px
            // from all four sides instead of only rounding the corners.
            Number rx = abs(P[0]) - p.RectInnerX;
            Number ry = abs(P[1]) - p.RectInnerY;
            Number lx = rx.Value > 0 ? rx : N(0);
            Number ly = ry.Value > 0 ? ry : N(0);
            Number m = rx.Value > ry.Value ? rx : ry;
            Number d = p.Rounding - Vector<2>{lx, ly}.norm2() - (m.Value < 0 ? m : N(0));
            return FadeAlpha(d, p);
        }
        return FadeAlpha(min(p.HalfX - abs(P[0]), p.HalfY - abs(P[1])), p);
    }

    case Geometries::Circle:
        return FadeAlpha(p.HalfX - P.norm2(), p);

    case Geometries::Ellipse:
    {
        Number t = sqrt(sq(P[0] * p.InvHalfX) + sq(P[1] * p.InvHalfY));
        return FadeAlpha((N(1) - t) * p.EllipseMinHalf, p);
    }

    case Geometries::Trapezoid:
    {
        // Isosceles trapezoid: Size = [bottom width, height], Angles = side slant (deg).
        // 0 at the bottom, 1 at the top; the half-widths and the slope are per-geometry.
        Number f = (P[1] + p.HalfY) * p.InvSizeY;
        Number hw = p.HalfX + p.TrapSlope * f;
        Number d = min(p.HalfY - abs(P[1]), hw - abs(P[0]));
        return FadeAlpha(d, p);
    }

    case Geometries::DoubleParabola:
    {
        return FadeAlpha(-(abs(P[0]) - p.SizeX + sq(P[1]) * p.ParabolaK), p);
    }

    case Geometries::Triangle:
    {
        // Equilateral (Size = side) by default; isosceles (Size = equal side, Angles =
        // apex angle in deg) when Angles is set. Centered: apex up, base horizontal.
        Number d = min(p.TriHalfH - P[1] - p.TriFactor * abs(P[0]), P[1] + p.TriHalfH);
        return FadeAlpha(d, p);
    }

    case Geometries::Polygon:
    case Geometries::Star:
    {
        Number R = p.SizeX;
        if (R.Value <= 0) return 0;
        int n = p.PointNumber < 3 ? 3 : p.PointNumber;
        if (n > 32) n = 32;
        Number r = P.norm2();
        if (r.Value <= 0) return FadeAlpha(R, p);
        Number theta = atan2(P[1], P[0]);
        if (theta.Value < 0) theta = theta + 2 * GetPI();

        const Number sector = p.PolySector;
        Number v = R;
        if (p.Shape == Geometries::Star)
        {
            // Star: 2n vertices alternating outer R / inner R*ratio (Angles or 0.5).
            uint16_t vi = (uint16_t)(theta * p.InvSector).ToInt();
            v = (vi & 1) ? p.StarInnerR : R;
        }
        Number ang = (theta - Number((theta * p.InvSector).ToInt()) * sector) - p.PolyHalfSector;
        // The per-pixel cos(ang) keeps a divide (it cannot be hoisted); everything else is
        // precomputed per geometry.
        Number d = v * p.PolyCosHalf / cos(ang) - r;
        return FadeAlpha(d, p);
    }

    case Geometries::Noise:
    {
        // Deterministic per-cell hash of (NoiseSeed, scaled coords) -> 0..255.
        int32_t x = (P[0] * p.NoiseInvX).ToInt();
        int32_t y = (P[1] * p.NoiseInvY).ToInt();
        uint32_t h = p.NoiseSeed;
        h ^= (uint32_t)(x * 2654435761);
        h *= 16777619;
        h ^= (uint32_t)(y * 1597334677);
        h *= 16777619;
        h ^= h >> 13;
        return (uint8_t)((h >> 8) & 0xFF);
    }

    default:
        return 0;
    }
}
