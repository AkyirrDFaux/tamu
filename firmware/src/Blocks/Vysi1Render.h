#pragma once

// The render pipeline (the out-of-line Vysi1Display member definitions).
//
// Part of the LED display block; included by
// Blocks/Vysi1Display.h in dependency order.

#include "Blocks/Vysi1Layout.h"

// Promotes a 2x3 affine to a 3x3 homogeneous matrix (docs store "Matrix 2x3",
// internally we compose 3x3 homogeneous transforms).
inline Matrix<3, 3> Vysi1Display::PromoteAffine(const Matrix<2, 3> &m)
{
    Matrix<3, 3> out = Matrix<3, 3>::Identity();
    out(0, 0) = m(0, 0);
    out(0, 1) = m(0, 1);
    out(0, 2) = m(0, 2);
    out(1, 0) = m(1, 0);
    out(1, 1) = m(1, 1);
    out(1, 2) = m(1, 2);
    return out;
}

// The screen transform shared by geometry and texture fields: the field's 2x3 Position
// composed with the display's base transform. Both passes previously spelled this out.
inline Matrix<3, 3> Vysi1Display::FieldTransform(const Matrix<2, 3> &pos)
{
    return PromoteAffine(pos) * BaseTransform();
}

// Resolves geometry field `field` and fills GeoMask[field][led] with each LED's alpha.
// Expensive per-LED shape math; only called when the block/offset/layout changed.
inline void Vysi1Display::RenderGeometryField(DynamicBlockDescriptor *block, uint16_t field, uint16_t slot)
{
    for (uint16_t i = 0; i < LedNum; i++)
        GeoMask[slot][i] = 0;

    GeometryParams p;
    p.Shape = block->GetKeyValue<Geometries>(field, (uint8_t)GeometryKey::Shape, DataType::Enum, Geometries::None);
    if (p.Shape == Geometries::None)
        return;

    // Combined transform: (Position) * (Offset * centering).
    Matrix<2, 3> pos = block->GetKeyValue<Matrix<2, 3>>(field, (uint8_t)GeometryKey::Position, DataType::Matrix, IdentityAffine23());
    Matrix<3, 3> combined = FieldTransform(pos);

    p.Rounding = block->GetKeyValue<Number>(field, (uint8_t)GeometryKey::Rounding, DataType::Number, N(0));
    p.Angles = block->GetKeyValue<Number>(field, (uint8_t)GeometryKey::Angles, DataType::Number, N(0));
    p.Fade = block->GetKeyValue<Number>(field, (uint8_t)GeometryKey::Fade, DataType::Number, N(1));
    // Alpha is documented 0..1: clamp so a >1 value cannot wrap the mask modulo 256.
    p.Alpha = LimitZeroToOne(block->GetKeyValue<Number>(field, (uint8_t)GeometryKey::Alpha, DataType::Number, N(1)));
    p.PointNumber = (uint8_t)ReadKeyInt(block, field, (uint8_t)GeometryKey::PointNumber, 0);
    p.NoiseSeed = (uint32_t)ReadKeyInt(block, field, (uint8_t)GeometryKey::NoiseSeed, 0);

    // Size: accept either a Number (side/diameter/radius/scale) or a Vector<2>
    // (width, height) regardless of the shape, so switching shapes or re-typing an
    // entry never silently leaves an invisible shape.
    {
        KeyResult kr = block->GetKey(field, (uint8_t)GeometryKey::Size);
        if (ValueInfoType(kr.meta.Type) == (uint16_t)DataType::Vector && kr.data_ptr)
        {
            Vector<2> size = *reinterpret_cast<const Vector<2> *>(kr.data_ptr);
            p.SizeX = size[0];
            p.SizeY = size[1];
        }
        else if (ValueInfoType(kr.meta.Type) == (uint16_t)DataType::Number && kr.data_ptr)
        {
            Number s = *reinterpret_cast<const Number *>(kr.data_ptr);
            p.SizeX = s;
            p.SizeY = s;
        }
        else
        {
            p.SizeX = p.SizeY = N(1);
        }
    }

    PrepareGeometry(p); // per-geometry invariants for the per-LED loop below

    // Fill is a constant 255 across every present LED; HalfFill's alpha depends only on the
    // transformed y (the combined matrix's second row). Both skip the full per-LED
    // Vector<3> transform + ShapeAlpha dispatch.
    if (p.Shape == Geometries::Fill)
    {
        uint8_t a = (uint8_t)((Number(255) * p.Alpha).RoundToInt());
        for (uint16_t led = 0; led < LedNum; led++)
            if (LedPresent[led])
                GeoMask[slot][led] = a;
        return;
    }
    if (p.Shape == Geometries::HalfFill)
    {
        for (uint16_t led = 0; led < LedNum; led++)
        {
            if (!LedPresent[led])
                continue;
            Number dy = combined(1, 0) * Number(LedX[led]) +
                        combined(1, 1) * Number(LedY[led]) + combined(1, 2);
            uint8_t a = FadeAlpha(dy, p);
            if (a == 0)
                continue;
            GeoMask[slot][led] = (uint8_t)((Number(a) * p.Alpha).RoundToInt());
        }
        return;
    }

    for (uint16_t led = 0; led < LedNum; led++)
    {
        if (!LedPresent[led])
            continue;
        Vector<3> pp = combined * Vector<3>{Number(LedX[led]), Number(LedY[led]), N(1)};
        Vector<2> p2 = {pp[0], pp[1]};
        uint8_t a = ShapeAlpha(p, p2);
        if (a == 0)
            continue;
        // a is already 0..255; scale by the geometry Alpha (0..1) back to 0..255.
        GeoMask[slot][led] = (uint8_t)((Number(a) * p.Alpha).RoundToInt());
    }
}

// Combines GeoMask[field] into the current mask with the field's operation.
inline void Vysi1Display::ApplyGeometryField(DynamicBlockDescriptor *block, uint16_t field, uint16_t slot)
{
    GeometryOperation op = block->GetKeyValue<GeometryOperation>(field, (uint8_t)GeometryKey::Operation, DataType::Enum, GeometryOperation::Replace);
    for (uint16_t led = 0; led < LedNum; led++)
    {
        uint16_t g = GeoMask[slot][led];
        uint16_t m = Mask[led];
        switch (op)
        {
        case GeometryOperation::Replace: m = g; break;
        case GeometryOperation::Add: m = m + g; if (m > 255) m = 255; break;
        case GeometryOperation::Cut: m = (m > g) ? m - g : 0; break;
        case GeometryOperation::Intersect: m = (m * g) >> 8; break;
        case GeometryOperation::XOR: m = (m > g) ? m - g : g - m; break;
        }
        Mask[led] = (uint8_t)m;
        if (m != 0) MaskAny = true;
    }
}

// Applies texture field `field` onto the buffer over the current mask, then (called by
// Render) the mask is reset. Textures: Fill, GradientLinear, GradientCircular; effects
// (InvertColour, HueShift, Contrast, Brightness) modify the buffer inside the mask.
inline void Vysi1Display::RenderTextureField(DynamicBlockDescriptor *block, uint16_t field)
{
    // Every texture and effect is masked by the geometry pass; skip when it produced no
    // non-zero alpha (nothing could be drawn).
    if (!MaskAny)
        return;

    Textures2D type = block->GetKeyValue<Textures2D>(field, (uint8_t)TextureKey::Type, DataType::Enum, Textures2D::None);
    if (type == Textures2D::None)
        return;

    // Texture-local transform for gradients: (Position) * (Offset * centering).
    Matrix<2, 3> pos = block->GetKeyValue<Matrix<2, 3>>(field, (uint8_t)TextureKey::Position, DataType::Matrix, IdentityAffine23());
    Matrix<3, 3> combined = FieldTransform(pos);

    switch (type)
    {
    case Textures2D::Fill:
    {
        ColourClass colour = block->GetKeyValue<ColourClass>(field, (uint8_t)TextureKey::Colour1, DataType::Colour, ColourClass(0, 0, 0, 0));
        for (uint16_t led = 0; led < LedNum; led++)
        {
            uint8_t a = Mask[led];
            if (a == 0)
                continue;
            Buffer[led].Layer(Linearise(colour), ByteToPercent(a));
        }
        break;
    }

    case Textures2D::GradientLinear:
    case Textures2D::GradientCircular:
    {
        ColourClass c1 = block->GetKeyValue<ColourClass>(field, (uint8_t)TextureKey::Colour1, DataType::Colour, ColourClass(0, 0, 0, 255));
        ColourClass c2 = block->GetKeyValue<ColourClass>(field, (uint8_t)TextureKey::Colour2, DataType::Colour, ColourClass(255, 255, 255, 255));
        Number extent = block->GetKeyValue<Number>(field, (uint8_t)TextureKey::Size, DataType::Number, N(1));
        if (extent.Value <= 0)
            extent = N(1);
        const Number invExtent = N(1) / extent; // constant per field: hoisted out of the loop
        for (uint16_t led = 0; led < LedNum; led++)
        {
            uint8_t a = Mask[led];
            if (a == 0 || !LedPresent[led])
                continue;
            Vector<3> pp = combined * Vector<3>{Number(LedX[led]), Number(LedY[led]), N(1)};
            Vector<2> p2 = {pp[0], pp[1]};
            Number t = (type == Textures2D::GradientLinear)
                ? LimitZeroToOne(p2[0] * invExtent + N(0.5))
                : LimitZeroToOne(p2.norm2() * invExtent);
            Buffer[led].Layer(Linearise(LerpColour(c1, c2, t)), ByteToPercent(a));
        }
        break;
    }

    // Textures and effects apply only within the ACTIVE mask (the region built by the
    // geometries); the mask persists across texture fields, so an effect following a
    // Fill modifies that Fill's area. A Fill before an effect is the expected setup.
    case Textures2D::InvertColour:
        for (uint16_t led = 0; led < LedNum; led++)
        {
            if (Mask[led] == 0)
                continue;
            Buffer[led].R = (uint8_t)(255 - Buffer[led].R);
            Buffer[led].G = (uint8_t)(255 - Buffer[led].G);
            Buffer[led].B = (uint8_t)(255 - Buffer[led].B);
        }
        break;

    case Textures2D::HueShift:
    {
        Number amount = block->GetKeyValue<Number>(field, (uint8_t)TextureKey::Amount, DataType::Number, N(0));
        for (uint16_t led = 0; led < LedNum; led++)
        {
            if (Mask[led] == 0)
                continue;
            ShiftHue(Buffer[led], amount);
        }
        break;
    }

    case Textures2D::Contrast:
    {
        Number amount = block->GetKeyValue<Number>(field, (uint8_t)TextureKey::Amount, DataType::Number, N(1));
        for (uint16_t led = 0; led < LedNum; led++)
        {
            if (Mask[led] == 0)
                continue;
            Buffer[led].R = LimitByte(((Number(Buffer[led].R) - N(128)) * amount + N(128)).RoundToInt());
            Buffer[led].G = LimitByte(((Number(Buffer[led].G) - N(128)) * amount + N(128)).RoundToInt());
            Buffer[led].B = LimitByte(((Number(Buffer[led].B) - N(128)) * amount + N(128)).RoundToInt());
        }
        break;
    }

    case Textures2D::Brightness:
    {
        Number amount = block->GetKeyValue<Number>(field, (uint8_t)TextureKey::Amount, DataType::Number, N(1));
        for (uint16_t led = 0; led < LedNum; led++)
        {
            if (Mask[led] == 0)
                continue;
            Buffer[led].R = LimitByte((Number(Buffer[led].R) * amount).RoundToInt());
            Buffer[led].G = LimitByte((Number(Buffer[led].G) * amount).RoundToInt());
            Buffer[led].B = LimitByte((Number(Buffer[led].B) * amount).RoundToInt());
        }
        break;
    }

    default:
        break;
    }
}

// The display's base screen transform: Offset (2x3, default rotation / 0,0 position)
// followed by centering the origin at the layout's centre.
inline Matrix<3, 3> Vysi1Display::BaseTransform()
{
    return PromoteAffine(Per.Offset) * Matrix<3, 3>::CreateTransform2D(N(0), {-(N(Lw) / N(2) - N(0.5)), -(N(Lh) / N(2) - N(0.5))}, {N(1), N(1)});
}

// Reads an integer (Index/Uint32) dictionary entry, else `def`.
inline int32_t Vysi1Display::ReadKeyInt(DynamicBlockDescriptor *block, uint16_t field, uint8_t key, int32_t def)
{
    KeyResult res = block->GetKey(field, key);
    if (res.data_ptr && res.data_len >= 4)
    {
        uint16_t t = ValueInfoType(res.meta.Type);
        if (t == (uint16_t)DataType::Index || t == (uint16_t)DataType::Uint32)
        {
            int32_t v;
            memcpy(&v, res.data_ptr, 4);
            return v;
        }
    }
    return def;
}

// Linear blend of two colours by t (0..1).
inline ColourClass Vysi1Display::LerpColour(const ColourClass &c1, const ColourClass &c2, Number t)
{
    ColourClass out;
    out.R = (uint8_t)LimitByte((Number(c1.R) + (Number(c2.R) - Number(c1.R)) * t).RoundToInt());
    out.G = (uint8_t)LimitByte((Number(c1.G) + (Number(c2.G) - Number(c1.G)) * t).RoundToInt());
    out.B = (uint8_t)LimitByte((Number(c1.B) + (Number(c2.B) - Number(c1.B)) * t).RoundToInt());
    out.A = (uint8_t)LimitByte((Number(c1.A) + (Number(c2.A) - Number(c1.A)) * t).RoundToInt());
    return out;
}

// Rotates the colour's hue by `hueDeg` (RGB -> HSV -> rotate -> RGB, fixed point).
inline void Vysi1Display::ShiftHue(ColourClass &c, Number hueDeg)
{
    Number r = ByteToPercent(c.R), g = ByteToPercent(c.G), b = ByteToPercent(c.B);
    Number mx = max(max(r, g), b), mn = min(min(r, g), b);
    Number delta = mx - mn;
    Number h = N(0), s = N(0), v = mx;
    if (delta.Value > 0)
    {
        s = delta / mx;
        Number seg;
        if (mx == r) seg = (g - b) / delta;
        else if (mx == g) seg = N(2) + (b - r) / delta;
        else seg = N(4) + (r - g) / delta;
        h = seg / N(6);
        if (h.Value < 0) h = h + N(1);
    }
    h = h + hueDeg / N(360);
    if (h.Value >= 1) h = h - N(1);
    if (h.Value < 0) h = h + N(1);

    if (s.Value <= 0)
    {
        c.R = c.G = c.B = (uint8_t)LimitByte((v * N(255)).RoundToInt());
        return;
    }

    Number h6 = h * N(6);
    int i = h6.ToInt();
    if (i < 0) i = 0;
    if (i > 5) i = 5;
    Number f = h6 - Number(i);
    Number p = v * (N(1) - s);
    Number q = v * (N(1) - s * f);
    Number tt = v * (N(1) - s * (N(1) - f));
    Number cr = N(0), cg = N(0), cb = N(0);
    switch (i)
    {
    case 0: cr = v; cg = tt; cb = p; break;
    case 1: cr = q; cg = v; cb = p; break;
    case 2: cr = p; cg = v; cb = tt; break;
    case 3: cr = p; cg = q; cb = v; break;
    case 4: cr = tt; cg = p; cb = v; break;
    default: cr = v; cg = p; cb = q; break;
    }
    c.R = (uint8_t)LimitByte((cr * N(255)).RoundToInt());
    c.G = (uint8_t)LimitByte((cg * N(255)).RoundToInt());
    c.B = (uint8_t)LimitByte((cb * N(255)).RoundToInt());
}

// Renders the configured render block into the LED buffer, applying brightness and gamma correction.
inline void Vysi1Display::Render()
{
    // Textures apply into the LED buffer ("texture always clears the buffer and
    // applies the texture in the given areas"), so the buffer must be cleared every
    // frame or pixels not covered by the current render would keep stale colours.
    memset((void *)Buffer, 0, LedNum * sizeof(ColourClass));

    if (Per.RenderBlock < 0 || Per.RenderBlock >= dynamic_block_registry.block_count)
        return;
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock((uint16_t)Per.RenderBlock);
    if (!block)
        return;

    // ---- geometry cache (mask): recompute only when the scene actually changed ----
    const Matrix<2, 3> &off = Per.Offset;
    bool offsetChanged = false;
    for (int i = 0; i < 6; i++)
        if (off.buffer.data[i].Value != CacheOffset[i]) { offsetChanged = true; break; }

    if (!CacheValid || block != CacheBlockPtr || offsetChanged ||
        block->generation != CacheBlockGen || LayoutGen != CacheLayoutGen)
    {
        for (int i = 0; i < 6; i++)
            CacheOffset[i] = off.buffer.data[i].Value;
        CacheBlockPtr = block;
        CacheBlockGen = block->generation;
        CacheLayoutGen = LayoutGen;
        CacheValid = true;

        // Collect the block's distinct field indexes (the render "parts") and classify
        // each once; the per-frame pass below reuses this instead of re-reading the type.
        uint8_t Fields[MaxCachedFields];
        CachedFieldCount = block->ListFields(Fields, MaxCachedFields);
        if (CachedFieldCount > MaxCachedFields) CachedFieldCount = MaxCachedFields;

        for (uint16_t fi = 0; fi < CachedFieldCount; fi++)
        {
            uint8_t f = Fields[fi];
            CachedFields[fi] = f;
            uint16_t t = ValueInfoType(block->GetKey(f, 0).meta.Type);
            if (t == (uint16_t)DataType::Geometry)
            {
                CachedFieldKind[fi] = 1;
                RenderGeometryField(block, f, fi);
            }
            else
            {
                CachedFieldKind[fi] = (t == (uint16_t)DataType::Texture) ? 2 : 0;
            }
        }
    }

    // ---- per-frame field pass: build the mask (geometries) and fill it (textures) ----
    memset((void *)Mask, 0, LedNum);
    MaskAny = false;
    for (uint16_t fi = 0; fi < CachedFieldCount; fi++)
    {
        uint8_t f = CachedFields[fi];
        if (CachedFieldKind[fi] == 1)
            ApplyGeometryField(block, f, fi);
        else if (CachedFieldKind[fi] == 2)
        {
            RenderTextureField(block, f);
            // The mask persists: geometries accumulate into it and every texture/effect
            // applies within the same active region (docs "mask and fill").
        }
    }

    // The render buffer already holds linear LED duty (colours are linearised as they are
    // filled, see Linearise), so only scale by Brightness here.
    Number brightness = Vol.Brightness;
    if (brightness < N(0)) brightness = N(0);
    // The layout file's brightness limit (Docs/Modules and blocks/LED display.md) is a 0-255
    // byte read as a percentage: it caps the configured brightness, which is the board's
    // current ceiling. 178 -> 70%, the same ceiling the brightness script uses.
    const Number limit((int32_t)(((uint32_t)BrightnessLimit * 100u + 127u) / 255u));
    if (brightness > limit) brightness = limit;
    // 256-scale so full brightness maps to exactly 255 after the >>8.
    uint32_t brightness_scale = (brightness >= 100) ? 256 : ((brightness * 256) / 100).ToInt();

    // Identity scale (>= 100%): (v * 256) >> 8 == v for every byte, so the whole pass
    // would just rewrite the buffer with itself.
    if (brightness_scale >= 256)
        return;

    for (uint16_t i = 0; i < LedNum; i++)
    {
        uint32_t r = (Buffer[i].R * brightness_scale) >> 8;
        uint32_t g = (Buffer[i].G * brightness_scale) >> 8;
        uint32_t b = (Buffer[i].B * brightness_scale) >> 8;

        Buffer[i].R = r > 255 ? 255 : r;
        Buffer[i].G = g > 255 ? 255 : g;
        Buffer[i].B = b > 255 ? 255 : b;
    }
}
