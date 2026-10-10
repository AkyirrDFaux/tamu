#pragma once

// The render pipeline (the out-of-line Vysi1Display member definitions).
//
// Part of the LED display block; included by
// Blocks/Vysi1Display.h in dependency order.

#include "Blocks/Vysi1Layout.h"

// Promotes a 2x3 affine to a 3x3 homogeneous matrix (docs store "Matrix 2x3",
// internally we compose 3x3 homogeneous transforms).
inline Matrix<3, 3> Vysi1Display::PromoteAffine(const Matrix<2, 3> &Mat)
{
    Matrix<3, 3> Out = Matrix<3, 3>::Identity();
    Out(0, 0) = Mat(0, 0);
    Out(0, 1) = Mat(0, 1);
    Out(0, 2) = Mat(0, 2);
    Out(1, 0) = Mat(1, 0);
    Out(1, 1) = Mat(1, 1);
    Out(1, 2) = Mat(1, 2);
    return Out;
}

// The screen transform shared by geometry and texture fields: the field's 2x3 Position
// composed with the display's base transform. Both passes previously spelled this out.
inline Matrix<3, 3> Vysi1Display::FieldTransform(const Matrix<2, 3> &Pos)
{
    return PromoteAffine(Pos) * BaseTransform();
}

// Resolves geometry field `field` and fills GeoMask[field][led] with each LED's alpha.
// Expensive per-LED shape math; only called when the block/offset/layout changed.
inline void Vysi1Display::RenderGeometryField(DynamicBlockDescriptor *Block, uint16_t Field, uint16_t Slot)
{
    for (uint16_t i = 0; i < LedNum; i++)
        GeoMask[Slot][i] = 0;

    GeometryParams Params;
    Params.Shape = Block->GetKeyValue<Geometries>(Field, (uint8_t)GeometryKey::Shape, DataType::Enum, Geometries::None);
    if (Params.Shape == Geometries::None)
        return;

    // Combined transform: (Position) * (Offset * centering).
    Matrix<2, 3> Pos = Block->GetKeyValue<Matrix<2, 3>>(Field, (uint8_t)GeometryKey::Position, DataType::Matrix, IdentityAffine23());
    Matrix<3, 3> Combined = FieldTransform(Pos);

    Params.Rounding = Block->GetKeyValue<Number>(Field, (uint8_t)GeometryKey::Rounding, DataType::Number, N(0));
    Params.Angles = Block->GetKeyValue<Number>(Field, (uint8_t)GeometryKey::Angles, DataType::Number, N(0));
    Params.Fade = Block->GetKeyValue<Number>(Field, (uint8_t)GeometryKey::Fade, DataType::Number, N(1));
    // Alpha is documented 0..1: clamp so a >1 value cannot wrap the mask modulo 256.
    Params.Alpha = LimitZeroToOne(Block->GetKeyValue<Number>(Field, (uint8_t)GeometryKey::Alpha, DataType::Number, N(1)));
    Params.PointNumber = (uint8_t)ReadKeyInt(Block, Field, (uint8_t)GeometryKey::PointNumber, 0);
    Params.NoiseSeed = (uint32_t)ReadKeyInt(Block, Field, (uint8_t)GeometryKey::NoiseSeed, 0);

    // Size: accept either a Number (side/diameter/radius/scale) or a Vector<2>
    // (width, height) regardless of the shape, so switching shapes or re-typing an
    // entry never silently leaves an invisible shape.
    {
        KeyResult Kr = Block->GetKey(Field, (uint8_t)GeometryKey::Size);
        if (ValueInfoType(Kr.Meta.Type) == (uint16_t)DataType::Vector && Kr.DataPtr)
        {
            Vector<2> Size = *reinterpret_cast<const Vector<2> *>(Kr.DataPtr);
            Params.SizeX = Size[0];
            Params.SizeY = Size[1];
        }
        else if (ValueInfoType(Kr.Meta.Type) == (uint16_t)DataType::Number && Kr.DataPtr)
        {
            Number Size = *reinterpret_cast<const Number *>(Kr.DataPtr);
            Params.SizeX = Size;
            Params.SizeY = Size;
        }
        else
        {
            Params.SizeX = Params.SizeY = N(1);
        }
    }

    PrepareGeometry(Params); // per-geometry invariants for the per-LED loop below

    // Fill is a constant 255 across every present LED; HalfFill's alpha depends only on the
    // transformed y (the combined matrix's second row). Both skip the full per-LED
    // Vector<3> transform + ShapeAlpha dispatch.
    if (Params.Shape == Geometries::Fill)
    {
        uint8_t Alpha = (uint8_t)((Number(255) * Params.Alpha).RoundToInt());
        for (uint16_t Led = 0; Led < LedNum; Led++)
            if (LedPresent[Led])
                GeoMask[Slot][Led] = Alpha;
        return;
    }
    if (Params.Shape == Geometries::HalfFill)
    {
        for (uint16_t Led = 0; Led < LedNum; Led++)
        {
            if (!LedPresent[Led])
                continue;
            Number Dy = Combined(1, 0) * Number(LedX[Led]) +
                        Combined(1, 1) * Number(LedY[Led]) + Combined(1, 2);
            uint8_t Alpha = FadeAlpha(Dy, Params);
            if (Alpha == 0)
                continue;
            GeoMask[Slot][Led] = (uint8_t)((Number(Alpha) * Params.Alpha).RoundToInt());
        }
        return;
    }

    for (uint16_t Led = 0; Led < LedNum; Led++)
    {
        if (!LedPresent[Led])
            continue;
        Vector<3> Pp = Combined * Vector<3>{Number(LedX[Led]), Number(LedY[Led]), N(1)};
        Vector<2> P2 = {Pp[0], Pp[1]};
        uint8_t Alpha = ShapeAlpha(Params, P2);
        if (Alpha == 0)
            continue;
        // a is already 0..255; scale by the geometry Alpha (0..1) back to 0..255.
        GeoMask[Slot][Led] = (uint8_t)((Number(Alpha) * Params.Alpha).RoundToInt());
    }
}

// Combines GeoMask[field] into the current mask with the field's operation.
inline void Vysi1Display::ApplyGeometryField(DynamicBlockDescriptor *Block, uint16_t Field, uint16_t Slot)
{
    GeometryOperation Op = Block->GetKeyValue<GeometryOperation>(Field, (uint8_t)GeometryKey::Operation, DataType::Enum, GeometryOperation::Replace);
    for (uint16_t Led = 0; Led < LedNum; Led++)
    {
        uint16_t Geo = GeoMask[Slot][Led];
        uint16_t MaskVal = Mask[Led];
        switch (Op)
        {
        case GeometryOperation::Replace: MaskVal = Geo; break;
        case GeometryOperation::Add: MaskVal = MaskVal + Geo; if (MaskVal > 255) MaskVal = 255; break;
        case GeometryOperation::Cut: MaskVal = (MaskVal > Geo) ? MaskVal - Geo : 0; break;
        case GeometryOperation::Intersect: MaskVal = (MaskVal * Geo) >> 8; break;
        case GeometryOperation::XOR: MaskVal = (MaskVal > Geo) ? MaskVal - Geo : Geo - MaskVal; break;
        }
        Mask[Led] = (uint8_t)MaskVal;
        if (MaskVal != 0) MaskAny = true;
    }
}

// Applies texture field `field` onto the buffer over the current mask, then (called by
// Render) the mask is reset. Textures: Fill, GradientLinear, GradientCircular; effects
// (InvertColour, HueShift, Contrast, Brightness) modify the buffer inside the mask.
inline void Vysi1Display::RenderTextureField(DynamicBlockDescriptor *Block, uint16_t Field)
{
    // Every texture and effect is masked by the geometry pass; skip when it produced no
    // non-zero alpha (nothing could be drawn).
    if (!MaskAny)
        return;

    Textures2D Type = Block->GetKeyValue<Textures2D>(Field, (uint8_t)TextureKey::Type, DataType::Enum, Textures2D::None);
    if (Type == Textures2D::None)
        return;

    // Texture-local transform for gradients: (Position) * (Offset * centering).
    Matrix<2, 3> Pos = Block->GetKeyValue<Matrix<2, 3>>(Field, (uint8_t)TextureKey::Position, DataType::Matrix, IdentityAffine23());
    Matrix<3, 3> Combined = FieldTransform(Pos);

    switch (Type)
    {
    case Textures2D::Fill:
    {
        ColourClass Colour = Block->GetKeyValue<ColourClass>(Field, (uint8_t)TextureKey::Colour1, DataType::Colour, ColourClass(0, 0, 0, 0));
        for (uint16_t Led = 0; Led < LedNum; Led++)
        {
            uint8_t Alpha = Mask[Led];
            if (Alpha == 0)
                continue;
            Buffer[Led].Layer(Linearise(Colour), ByteToPercent(Alpha));
        }
        break;
    }

    case Textures2D::GradientLinear:
    case Textures2D::GradientCircular:
    {
        ColourClass C1 = Block->GetKeyValue<ColourClass>(Field, (uint8_t)TextureKey::Colour1, DataType::Colour, ColourClass(0, 0, 0, 255));
        ColourClass C2 = Block->GetKeyValue<ColourClass>(Field, (uint8_t)TextureKey::Colour2, DataType::Colour, ColourClass(255, 255, 255, 255));
        // Size accepts a Number (uniform: scales both axes) or a Vector<2> (width, height),
        // the same two types a geometry's Size takes; a non-positive extent falls back to 1.
        Number ExtentX = N(1), ExtentY = N(1);
        {
            KeyResult Kr = Block->GetKey(Field, (uint8_t)TextureKey::Size);
            if (ValueInfoType(Kr.Meta.Type) == (uint16_t)DataType::Vector && Kr.DataPtr)
            {
                Vector<2> Size = *reinterpret_cast<const Vector<2> *>(Kr.DataPtr);
                ExtentX = Size[0];
                ExtentY = Size[1];
            }
            else if (ValueInfoType(Kr.Meta.Type) == (uint16_t)DataType::Number && Kr.DataPtr)
            {
                ExtentX = ExtentY = *reinterpret_cast<const Number *>(Kr.DataPtr);
            }
        }
        if (ExtentX.Value <= 0) ExtentX = N(1);
        if (ExtentY.Value <= 0) ExtentY = N(1);
        const Number InvExtentX = N(1) / ExtentX; // constant per field: hoisted out of the loop
        const Number InvExtentY = N(1) / ExtentY;
        for (uint16_t Led = 0; Led < LedNum; Led++)
        {
            uint8_t Alpha = Mask[Led];
            if (Alpha == 0 || !LedPresent[Led])
                continue;
            Vector<3> Pp = Combined * Vector<3>{Number(LedX[Led]), Number(LedY[Led]), N(1)};
            Vector<2> P2 = {Pp[0], Pp[1]};
            // Linear uses the width alone; circular is an ellipse over both axes (a circle
            // when they are equal, e.g. a Number Size).
            Number Blend = (Type == Textures2D::GradientLinear)
                ? LimitZeroToOne(P2[0] * InvExtentX + N(0.5))
                : LimitZeroToOne(Vector<2>{P2[0] * InvExtentX, P2[1] * InvExtentY}.Norm2());
            Buffer[Led].Layer(Linearise(LerpColour(C1, C2, Blend)), ByteToPercent(Alpha));
        }
        break;
    }

    // Textures and effects apply only within the ACTIVE mask (the region built by the
    // geometries); the mask persists across texture fields, so an effect following a
    // Fill modifies that Fill's area. A Fill before an effect is the expected setup.
    case Textures2D::InvertColour:
        for (uint16_t Led = 0; Led < LedNum; Led++)
        {
            if (Mask[Led] == 0)
                continue;
            Buffer[Led].Red = (uint8_t)(255 - Buffer[Led].Red);
            Buffer[Led].Green = (uint8_t)(255 - Buffer[Led].Green);
            Buffer[Led].Blue = (uint8_t)(255 - Buffer[Led].Blue);
        }
        break;

    case Textures2D::HueShift:
    {
        Number Amount = Block->GetKeyValue<Number>(Field, (uint8_t)TextureKey::Amount, DataType::Number, N(0));
        for (uint16_t Led = 0; Led < LedNum; Led++)
        {
            if (Mask[Led] == 0)
                continue;
            ShiftHue(Buffer[Led], Amount);
        }
        break;
    }

    case Textures2D::Contrast:
    {
        Number Amount = Block->GetKeyValue<Number>(Field, (uint8_t)TextureKey::Amount, DataType::Number, N(1));
        for (uint16_t Led = 0; Led < LedNum; Led++)
        {
            if (Mask[Led] == 0)
                continue;
            Buffer[Led].Red = LimitByte(((Number(Buffer[Led].Red) - N(128)) * Amount + N(128)).RoundToInt());
            Buffer[Led].Green = LimitByte(((Number(Buffer[Led].Green) - N(128)) * Amount + N(128)).RoundToInt());
            Buffer[Led].Blue = LimitByte(((Number(Buffer[Led].Blue) - N(128)) * Amount + N(128)).RoundToInt());
        }
        break;
    }

    case Textures2D::Brightness:
    {
        Number Amount = Block->GetKeyValue<Number>(Field, (uint8_t)TextureKey::Amount, DataType::Number, N(1));
        for (uint16_t Led = 0; Led < LedNum; Led++)
        {
            if (Mask[Led] == 0)
                continue;
            Buffer[Led].Red = LimitByte((Number(Buffer[Led].Red) * Amount).RoundToInt());
            Buffer[Led].Green = LimitByte((Number(Buffer[Led].Green) * Amount).RoundToInt());
            Buffer[Led].Blue = LimitByte((Number(Buffer[Led].Blue) * Amount).RoundToInt());
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
inline int32_t Vysi1Display::ReadKeyInt(DynamicBlockDescriptor *Block, uint16_t Field, uint8_t Key, int32_t Def)
{
    KeyResult Res = Block->GetKey(Field, Key);
    if (Res.DataPtr && Res.DataLen >= 4)
    {
        uint16_t TypeId = ValueInfoType(Res.Meta.Type);
        if (TypeId == (uint16_t)DataType::Index || TypeId == (uint16_t)DataType::Uint32)
        {
            int32_t Raw;
            memcpy(&Raw, Res.DataPtr, 4);
            return Raw;
        }
    }
    return Def;
}

// Linear blend of two colours by t (0..1).
inline ColourClass Vysi1Display::LerpColour(const ColourClass &C1, const ColourClass &C2, Number Factor)
{
    ColourClass Out;
    Out.Red = (uint8_t)LimitByte((Number(C1.Red) + (Number(C2.Red) - Number(C1.Red)) * Factor).RoundToInt());
    Out.Green = (uint8_t)LimitByte((Number(C1.Green) + (Number(C2.Green) - Number(C1.Green)) * Factor).RoundToInt());
    Out.Blue = (uint8_t)LimitByte((Number(C1.Blue) + (Number(C2.Blue) - Number(C1.Blue)) * Factor).RoundToInt());
    Out.Alpha = (uint8_t)LimitByte((Number(C1.Alpha) + (Number(C2.Alpha) - Number(C1.Alpha)) * Factor).RoundToInt());
    return Out;
}

// Rotates the colour's hue by `hueDeg` (RGB -> HSV -> rotate -> RGB, fixed point).
inline void Vysi1Display::ShiftHue(ColourClass &Colour, Number HueDeg)
{
    Number Red = ByteToPercent(Colour.Red), Green = ByteToPercent(Colour.Green), Blue = ByteToPercent(Colour.Blue);
    Number Mx = max(max(Red, Green), Blue), Mn = min(min(Red, Green), Blue);
    Number Delta = Mx - Mn;
    Number Hue = N(0), Sat = N(0), Val = Mx;
    if (Delta.Value > 0)
    {
        Sat = Delta / Mx;
        Number Seg;
        if (Mx == Red) Seg = (Green - Blue) / Delta;
        else if (Mx == Green) Seg = N(2) + (Blue - Red) / Delta;
        else Seg = N(4) + (Red - Green) / Delta;
        Hue = Seg / N(6);
        if (Hue.Value < 0) Hue = Hue + N(1);
    }
    Hue = Hue + HueDeg / N(360);
    if (Hue.Value >= 1) Hue = Hue - N(1);
    if (Hue.Value < 0) Hue = Hue + N(1);

    if (Sat.Value <= 0)
    {
        Colour.Red = Colour.Green = Colour.Blue = (uint8_t)LimitByte((Val * N(255)).RoundToInt());
        return;
    }

    Number H6 = Hue * N(6);
    int i = H6.ToInt();
    if (i < 0) i = 0;
    if (i > 5) i = 5;
    Number Fch = H6 - Number(i);
    Number Pch = Val * (N(1) - Sat);
    Number Qch = Val * (N(1) - Sat * Fch);
    Number Tt = Val * (N(1) - Sat * (N(1) - Fch));
    Number Cr = N(0), Cg = N(0), Cb = N(0);
    switch (i)
    {
    case 0: Cr = Val; Cg = Tt; Cb = Pch; break;
    case 1: Cr = Qch; Cg = Val; Cb = Pch; break;
    case 2: Cr = Pch; Cg = Val; Cb = Tt; break;
    case 3: Cr = Pch; Cg = Qch; Cb = Val; break;
    case 4: Cr = Tt; Cg = Pch; Cb = Val; break;
    default: Cr = Val; Cg = Pch; Cb = Qch; break;
    }
    Colour.Red = (uint8_t)LimitByte((Cr * N(255)).RoundToInt());
    Colour.Green = (uint8_t)LimitByte((Cg * N(255)).RoundToInt());
    Colour.Blue = (uint8_t)LimitByte((Cb * N(255)).RoundToInt());
}

// Renders the configured render block into the LED buffer, applying brightness and gamma correction.
inline void Vysi1Display::Render()
{
    // Textures apply into the LED buffer ("texture always clears the buffer and
    // applies the texture in the given areas"), so the buffer must be cleared every
    // frame or pixels not covered by the current render would keep stale colours.
    memset((void *)Buffer, 0, LedNum * sizeof(ColourClass));

    if (Per.RenderBlock < 0 || Per.RenderBlock >= DynamicBlockRegistry.BlockCount)
        return;
    DynamicBlockDescriptor *Block = DynamicBlockRegistry.GetBlock((uint16_t)Per.RenderBlock);
    if (!Block)
        return;

    // ---- geometry cache (mask): recompute only when the scene actually changed ----
    const Matrix<2, 3> &Off = Per.Offset;
    bool OffsetChanged = false;
    for (int i = 0; i < 6; i++)
        if (Off.Buffer.Data[i].Value != CacheOffset[i]) { OffsetChanged = true; break; }

    if (!CacheValid || Block != CacheBlockPtr || OffsetChanged ||
        Block->Generation != CacheBlockGen || LayoutGen != CacheLayoutGen)
    {
        for (int i = 0; i < 6; i++)
            CacheOffset[i] = Off.Buffer.Data[i].Value;
        CacheBlockPtr = Block;
        CacheBlockGen = Block->Generation;
        CacheLayoutGen = LayoutGen;
        CacheValid = true;

        // Collect the block's distinct field indexes (the render "parts") and classify
        // each once; the per-frame pass below reuses this instead of re-reading the type.
        uint8_t Fields[MaxCachedFields];
        CachedFieldCount = Block->ListFields(Fields, MaxCachedFields);
        if (CachedFieldCount > MaxCachedFields) CachedFieldCount = MaxCachedFields;

        for (uint16_t Fi = 0; Fi < CachedFieldCount; Fi++)
        {
            uint8_t Field = Fields[Fi];
            CachedFields[Fi] = Field;
            uint16_t TypeId = ValueInfoType(Block->GetKey(Field, 0).Meta.Type);
            if (TypeId == (uint16_t)DataType::Geometry)
            {
                CachedFieldKind[Fi] = 1;
                RenderGeometryField(Block, Field, Fi);
            }
            else
            {
                CachedFieldKind[Fi] = (TypeId == (uint16_t)DataType::Texture) ? 2 : 0;
            }
        }
    }

    // ---- per-frame field pass: build the mask (geometries) and fill it (textures) ----
    memset((void *)Mask, 0, LedNum);
    MaskAny = false;
    for (uint16_t Fi = 0; Fi < CachedFieldCount; Fi++)
    {
        uint8_t Field = CachedFields[Fi];
        if (CachedFieldKind[Fi] == 1)
            ApplyGeometryField(Block, Field, Fi);
        else if (CachedFieldKind[Fi] == 2)
        {
            RenderTextureField(Block, Field);
            // The mask persists: geometries accumulate into it and every texture/effect
            // applies within the same active region (docs "mask and fill").
        }
    }

    // The render buffer already holds linear LED duty (colours are linearised as they are
    // filled, see Linearise), so only scale by Brightness here.
    Number Brightness = Vol.Brightness;
    if (Brightness < N(0)) Brightness = N(0);
    if (Brightness > N(100)) Brightness = N(100);
    // The layout file's brightness limit (Docs/Modules and blocks/LED display.md) is a 0-255
    // byte read as a percentage: it is the physical ceiling the board's current allows. The
    // register holds 0-100 % and maps linearly onto it, so 100 % reaches the layout maximum
    // (178 -> 70 %).
    const Number Limit((int32_t)(((uint32_t)BrightnessLimit * 100u + 127u) / 255u));
    Brightness = (Brightness * Limit) / 100;
    // 256-scale so full brightness maps to exactly 255 after the >>8.
    uint32_t BrightnessScale = (Brightness >= 100) ? 256 : ((Brightness * 256) / 100).ToInt();

    // Identity scale (>= 100%): (v * 256) >> 8 == v for every byte, so the whole pass
    // would just rewrite the buffer with itself.
    if (BrightnessScale >= 256)
        return;

    for (uint16_t i = 0; i < LedNum; i++)
    {
        uint32_t Red = (Buffer[i].Red * BrightnessScale) >> 8;
        uint32_t Green = (Buffer[i].Green * BrightnessScale) >> 8;
        uint32_t Blue = (Buffer[i].Blue * BrightnessScale) >> 8;

        Buffer[i].Red = Red > 255 ? 255 : Red;
        Buffer[i].Green = Green > 255 ? 255 : Green;
        Buffer[i].Blue = Blue > 255 ? 255 : Blue;
    }
}
