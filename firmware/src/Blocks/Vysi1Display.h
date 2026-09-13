// Gamma = 1.8

#include "Blocks/Render.h"
#include "Core/Functions/Memory.h"
#include "Core/Types/Matrix.h"
#include "Core/Types/Vector.h"

const uint8_t GammaTable[256] = {
    0, 12, 17, 22, 25, 29, 32, 35, 37, 40, 42, 44, 47, 49, 51, 53,
    55, 57, 58, 60, 62, 64, 65, 67, 69, 70, 72, 73, 75, 76, 78, 79,
    80, 82, 83, 85, 86, 87, 89, 90, 91, 92, 94, 95, 96, 97, 98, 100,
    101, 102, 103, 104, 105, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117,
    118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133,
    134, 135, 136, 137, 138, 139, 139, 140, 141, 142, 143, 144, 145, 146, 146, 147,
    148, 149, 150, 151, 152, 152, 153, 154, 155, 156, 157, 157, 158, 159, 160, 161,
    161, 162, 163, 164, 165, 165, 166, 167, 168, 169, 169, 170, 171, 172, 172, 173,
    174, 175, 175, 176, 177, 178, 178, 179, 180, 181, 181, 182, 183, 183, 184, 185,
    186, 186, 187, 188, 188, 189, 190, 191, 191, 192, 193, 193, 194, 195, 195, 196,
    197, 198, 198, 199, 200, 200, 201, 202, 202, 203, 204, 204, 205, 206, 206, 207,
    208, 208, 209, 209, 210, 211, 211, 212, 213, 213, 214, 215, 215, 216, 217, 217,
    218, 218, 219, 220, 220, 221, 222, 222, 223, 223, 224, 225, 225, 226, 226, 227,
    228, 228, 229, 230, 230, 231, 231, 232, 233, 233, 234, 234, 235, 236, 236, 237,
    237, 238, 238, 239, 240, 240, 241, 241, 242, 243, 243, 244, 244, 245, 245, 246,
    247, 247, 248, 248, 249, 249, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255};

const uint8_t LayoutVysiv1_0[10 * 11]{
    0, 0, 0, 28, 29, 48, 49, 68, 0, 0, 0,
    0, 0, 11, 27, 30, 47, 50, 67, 69, 0, 0,
    0, 10, 12, 26, 31, 46, 51, 66, 70, 82, 0,
    1, 9, 13, 25, 32, 45, 52, 65, 71, 81, 83,
    2, 8, 14, 24, 33, 44, 53, 64, 72, 80, 84,
    3, 7, 15, 23, 34, 43, 54, 63, 73, 79, 85,
    4, 6, 16, 22, 35, 42, 55, 62, 74, 78, 86,
    0, 5, 17, 21, 36, 41, 56, 61, 75, 77, 0,
    0, 0, 18, 20, 37, 40, 57, 60, 76, 0, 0,
    0, 0, 0, 19, 38, 39, 58, 59, 0, 0, 0};

// Preloads the Vysi v1.0 layout file into storage on first boot (the project's
// layouts/ directory holds the same bytes). Only created when absent, so a user's
// customized layout is never overwritten. File format per Docs/Modules/LED display.md:
// u8 width, u8 height, then w*h u16 LE 0-based LED indices (0xFFFF = unused).
inline void PreloadVysiLayout()
{
    // Heal devices flashed with the first (buggy) preload: it wrote the name as
    // "VYSIV1 \0" (NUL in byte 7) instead of the space-padded form, leaving an
    // orphan entry that the app cannot delete (its delete pads with spaces).
    const char legacy[8] = "VYSIV1 ";
    if (Storage.FileExists(legacy) != 0xFFFFFFFF)
        Storage.DeleteFile(legacy);

    char name[8];
    PackName("VYSIV1", name); // space-padded 8-byte storage name ("VYSIV1  ")
    if (Storage.FileExists(name) != 0xFFFFFFFF)
        return; // already present

    uint8_t buf[2 + 11 * 10 * 2];
    buf[0] = 11; // width
    buf[1] = 10; // height
    uint16_t *idx = reinterpret_cast<uint16_t *>(buf + 2);
    for (uint32_t i = 0; i < 11 * 10; i++)
        idx[i] = (LayoutVysiv1_0[i] == 0) ? 0xFFFF : (uint16_t)(LayoutVysiv1_0[i] - 1);

    if (!Storage.CreateFile(name, sizeof(buf)))
        return;
    Storage.WriteToFile(name, 0, sizeof(buf), (const char *)buf);
}

// Identity 2x3 affine ([1 0 0; 0 1 0]), the default Offset transform.
static inline Matrix<2, 3> IdentityAffine23()
{
    Matrix<2, 3> m;
    m(0, 0) = Number(1);
    m(0, 1) = Number(0);
    m(0, 2) = Number(0);
    m(1, 0) = Number(0);
    m(1, 1) = Number(1);
    m(1, 2) = Number(0);
    return m;
}

struct Vysi1Struct
{
    Number Brightness = 30; //%
    Matrix<2, 3> Offset = IdentityAffine23(); // 2x3 transformation (0,0 position + rotation)
    int32_t RenderBlock = -1; // Signed index into dynamic_block_registry; -1 = none (invalid)
    // Layout File Name: plain 8-char storage file name, space padded. Default is blank =
    // built-in default layout. Written via trigger, which loads the layout file
    // immediately (write is rejected if the file cannot be loaded).
    char LayoutFile[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    Number RefreshRate;     // Out: achieved render rate in FPS (averaged)
};

const BlockMeta Vysi1_Map[] = {
    {DataType::Number | FieldFlags::None, 0x00, sizeof(Number)},      // Brightness
    {DataType::Matrix | FieldFlags::Persistent, 0x00, sizeof(Matrix<2, 3>)},// Offset (2x3)
    {DataType::Index | FieldFlags::Persistent, 0x00, sizeof(int32_t)},     // Render Block Index (signed, -1 = none)
    {DataType::String | FieldFlags::Trigger | FieldFlags::Persistent, 0x00, 8}, // Layout File Name
    {DataType::Number | FieldFlags::ReadOnly, 0x00, sizeof(Number)},  // Refresh Rate
};

// Renders the configured render block into the LED buffer, applying brightness and gamma correction.
// Performance-oriented rewrite: geometry contributions are cached per block generation
// (mask cache), so an unchanged scene costs one buffer clear + a per-LED fill pass per frame.
class Vysi1Display
{
public:
    static const uint32_t MaxLayoutEntries = 256;
    static const uint32_t LedNum = 86;
    static const uint32_t MaxCachedFields = 8;

    Vysi1Struct Data;
    uint16_t Layout[MaxLayoutEntries];
    uint8_t Lw = 11;
    uint8_t Lh = 10;
    ColourClass Buffer[LedNum];

    // LED-centric screen coordinates (rebuilt when a layout loads). Screen y = 0 at
    // the bottom; the layout array is row-first with its first row on top.
    uint8_t LedX[LedNum];
    uint8_t LedY[LedNum];
    bool LedPresent[LedNum];
    uint32_t LayoutGen = 0;

    // Mask: per-LED alpha (0..255) of the current mask section. Textures consume and
    // reset it; geometries accumulate into it.
    uint8_t Mask[LedNum];

    // Cached per-field geometry alpha (0..255), invalidated on block/offset/layout change.
    uint8_t GeoMask[MaxCachedFields][LedNum];
    DynamicBlockDescriptor *CacheBlockPtr = nullptr;
    uint32_t CacheBlockGen = 0xFFFFFFFF;
    int32_t CacheOffset[6] = {0, 0, 0, 0, 0, 0};
    uint32_t CacheLayoutGen = 0xFFFFFFFF;
    bool CacheValid = false;

    Vysi1Display() { LoadDefaultLayout(); }

    void Render();
    void RenderGeometryField(DynamicBlockDescriptor *block, uint16_t field);
    void ApplyGeometryField(DynamicBlockDescriptor *block, uint16_t field);
    void RenderTextureField(DynamicBlockDescriptor *block, uint16_t field);
    uint8_t ShapeAlpha(Geometries shape, const Vector<2> &P, Number hx, Number hy, Number fade);
    uint8_t FadeAlpha(Number distance, Number fade);
    Matrix<3, 3> PromoteAffine(const Matrix<2, 3> &m);
    Matrix<2, 3> IdentityAffine();

    // Rebuilds the per-LED screen-coordinate table from the current layout and bumps
    // LayoutGen (invalidates the geometry cache).
    void RebuildLedTable()
    {
        for (uint32_t i = 0; i < LedNum; i++)
        {
            LedPresent[i] = false;
            LedX[i] = 0;
            LedY[i] = 0;
        }
        for (uint32_t r = 0; r < (uint32_t)Lw * Lh && r < MaxLayoutEntries; r++)
        {
            uint16_t idx = Layout[r];
            if (idx == 0xFFFF || idx >= LedNum)
                continue;
            LedX[idx] = (uint8_t)(r % Lw);
            LedY[idx] = (uint8_t)(Lh - 1 - (r / Lw)); // screen y, 0 at bottom
            LedPresent[idx] = true;
        }
        LayoutGen++;
    }

    // Reverts to the compiled-in default layout (11x10, 0=missing converted to FFFF).
    void LoadDefaultLayout()
    {
        Lw = 11;
        Lh = 10;
        for (uint32_t i = 0; i < MaxLayoutEntries; i++)
            Layout[i] = 0xFFFF;
        for (uint32_t i = 0; i < Lw * Lh && i < MaxLayoutEntries; i++)
            Layout[i] = (LayoutVysiv1_0[i] == 0) ? 0xFFFF : (uint16_t)(LayoutVysiv1_0[i] - 1);
        RebuildLedTable();
    }

    // Loads the layout file named by Data.LayoutFile (8-char storage name form).
    // Rejects files that are malformed or whose LED indexes exceed this display.
    bool LoadLayoutFromStorage()
    {
        // Blank name (all spaces) -> built-in default layout.
        bool empty = true;
        for (int i = 0; i < 8; i++)
            if (Data.LayoutFile[i] != ' ' && Data.LayoutFile[i] != '\0') empty = false;
        if (empty)
        {
            LoadDefaultLayout();
            return true;
        }

        char n8[8];
        PackName(Data.LayoutFile, n8); // normalize to space-padded form
        uint32_t off, size;
        if (!Storage.GetFileInfo(n8, &off, &size))
            return false;
        if (size < 2)
            return false;

        uint8_t hdr[2]; // width, height
        Storage_FlashRead(off, hdr, 2);
        uint32_t entries = (uint32_t)hdr[0] * hdr[1];
        if (hdr[0] == 0 || hdr[1] == 0 || entries > MaxLayoutEntries ||
            size < 2 + entries * 2)
            return false;

        Storage_FlashRead(off + 2, (void *)Layout, entries * 2);
        for (uint32_t i = 0; i < entries; i++)
            if (Layout[i] != 0xFFFF && Layout[i] >= LedNum)
                Layout[i] = 0xFFFF; // index beyond this display's chain
        Lw = hdr[0];
        Lh = hdr[1];
        RebuildLedTable();
        return true;
    }
};

// Layout-file write trigger: stores the new Name and loads the layout file immediately
// so the stored name always matches the layout in use. `block.Data` is the first member
// of the owning Vysi1Display instance (Main.h registers &DisplayN.Data), which recovers
// the per-display runtime layout.
inline bool OnVysi1FieldWrite(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t len)
{
    if (index != 3 || len != 8)
        return false;
    auto* disp = reinterpret_cast<Vysi1Display*>(block.Data); // Data is the first member
    // Validate against the NEW name before committing it, so a failed layout
    // load leaves the stored name matching the layout actually in use
    // ("stored value equals applied value"). LoadLayoutFromStorage reads
    // Data.LayoutFile, so load through a temporary and only copy on success.
    char pending[8];
    memcpy(pending, data, 8);
    char saved[8];
    memcpy(saved, disp->Data.LayoutFile, 8);
    memcpy(disp->Data.LayoutFile, pending, 8);
    bool ok = disp->LoadLayoutFromStorage();
    if (!ok)
        memcpy(disp->Data.LayoutFile, saved, 8); // revert the RAM field
    return ok;
}

const FieldTrigger Vysi1_Triggers[] = {
    nullptr,
    nullptr,
    nullptr,
    OnVysi1FieldWrite,
    nullptr,
};

const uint16_t Vysi1_Offsets[] = {
    0,                                    // Brightness (Number, 4B)
    4,                                    // Offset (Matrix<2,3>, 28B)
    32,                                   // RenderBlock (int32, 4B)
    36,                                   // LayoutFile (String, 8B)
    44,                                   // RefreshRate (Number, 4B)
};

const BlockSchema Vysi1_Schema = {
    .Map = Vysi1_Map,
    .Triggers = Vysi1_Triggers,
    .Offsets = Vysi1_Offsets,
    .Type = BlockType::Vysi1Display,
    .MapCount = sizeof(Vysi1_Map) / sizeof(BlockMeta),
};

// Identity 2x3 affine ([1 0 0; 0 1 0]).
inline Matrix<2, 3> Vysi1Display::IdentityAffine()
{
    Matrix<2, 3> m;
    m(0, 0) = Number(1);
    m(0, 1) = Number(0);
    m(0, 2) = Number(0);
    m(1, 0) = Number(0);
    m(1, 1) = Number(1);
    m(1, 2) = Number(0);
    return m;
}

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

// Converts a signed edge distance (pixels, positive inside) into a 0..255 alpha with a
// `fade`-pixel soft edge (alpha 1 at distance >= fade/2, 0 at <= -fade/2). Fade <= 0 = hard edge.
inline uint8_t Vysi1Display::FadeAlpha(Number distance, Number fade)
{
    if (fade.Value <= 0)
        return (distance.Value >= 0) ? 255 : 0;
    return (uint8_t)(LimitZeroToOne(distance / fade + N(0.5)) * N(255)).RoundToInt();
}

// Computes the 0..255 alpha of `shape` at point P (shape-local space) for the shapes
// implemented so far; unhandled shapes return 0.
inline uint8_t Vysi1Display::ShapeAlpha(Geometries shape, const Vector<2> &P, Number hx, Number hy, Number fade)
{
    switch (shape)
    {
    case Geometries::Square:
    case Geometries::Rectangle:
        // Distance to the nearest edge (positive inside).
        return FadeAlpha(min(hx - abs(P[0]), hy - abs(P[1])), fade);
    default:
        return 0; // shapes beyond the initial milestone (expand from here)
    }
}

// Resolves geometry field `field` and fills GeoMask[field][led] with each LED's alpha.
// Expensive per-LED shape math; only called when the block/offset/layout changed.
inline void Vysi1Display::RenderGeometryField(DynamicBlockDescriptor *block, uint16_t field)
{
    for (uint16_t i = 0; i < LedNum; i++)
        GeoMask[field][i] = 0;

    Geometries shape = block->GetKeyValue<Geometries>(field, (uint8_t)GeometryKey::Shape, DataType::Enum, Geometries::None);
    if (shape == Geometries::None)
        return;

    // Combined transform: (Position) * (Offset * centering). Offset is the static
    // block's 2x3 "default rotation / 0,0 position", promoted to 3x3 homogeneous; the
    // centering puts the origin at the layout's centre.
    Matrix<2, 3> pos = block->GetKeyValue<Matrix<2, 3>>(field, (uint8_t)GeometryKey::Position, DataType::Matrix, IdentityAffine());
    Matrix<3, 3> local = PromoteAffine(pos);
    Matrix<3, 3> base = PromoteAffine(Data.Offset) * Matrix<3, 3>::CreateTransform2D(N(0), {-(N(Lw) / N(2) - N(0.5)), -(N(Lh) / N(2) - N(0.5))}, {N(1), N(1)});
    Matrix<3, 3> combined = local * base;

    // Size: Square takes a Number (side), Rectangle takes a Vector<2> (w, h).
    Number sx = N(0), sy = N(0);
    if (shape == Geometries::Square)
    {
        sx = sy = block->GetKeyValue<Number>(field, (uint8_t)GeometryKey::Size, DataType::Number, N(1));
    }
    else if (shape == Geometries::Rectangle)
    {
        Vector<2> size = block->GetKeyValue<Vector<2>>(field, (uint8_t)GeometryKey::Size, DataType::Vector, Vector<2>());
        sx = size[0];
        sy = size[1];
    }

    Number fade = block->GetKeyValue<Number>(field, (uint8_t)GeometryKey::Fade, DataType::Number, N(1));
    Number alpha = block->GetKeyValue<Number>(field, (uint8_t)GeometryKey::Alpha, DataType::Number, N(1));
    Number hx = sx / N(2);
    Number hy = sy / N(2);

    for (uint16_t led = 0; led < LedNum; led++)
    {
        if (!LedPresent[led])
            continue;
        Vector<3> p = combined * Vector<3>{Number(LedX[led]), Number(LedY[led]), N(1)};
        Vector<2> p2 = {p[0], p[1]};
        uint8_t a = ShapeAlpha(shape, p2, hx, hy, fade);
        if (a == 0)
            continue;
        // a is already 0..255; scale by the geometry Alpha (0..1) back to 0..255.
        GeoMask[field][led] = (uint8_t)((Number(a) * alpha).RoundToInt());
    }
}

// Combines GeoMask[field] into the current mask with the field's operation.
inline void Vysi1Display::ApplyGeometryField(DynamicBlockDescriptor *block, uint16_t field)
{
    GeometryOperation op = block->GetKeyValue<GeometryOperation>(field, (uint8_t)GeometryKey::Operation, DataType::Enum, GeometryOperation::Replace);
    for (uint16_t led = 0; led < LedNum; led++)
    {
        uint16_t g = GeoMask[field][led];
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
    }
}

// Applies texture field `field` onto the buffer over the current mask, then (called by
// Render) the mask is reset. Initial milestone: Fill (solid colour); the rest are no-ops.
inline void Vysi1Display::RenderTextureField(DynamicBlockDescriptor *block, uint16_t field)
{
    Textures2D type = block->GetKeyValue<Textures2D>(field, (uint8_t)TextureKey::Type, DataType::Enum, Textures2D::None);
    if (type != Textures2D::Fill)
        return;

    ColourClass colour = block->GetKeyValue<ColourClass>(field, (uint8_t)TextureKey::Colour1, DataType::Colour, ColourClass(0, 0, 0, 0));
    for (uint16_t led = 0; led < LedNum; led++)
    {
        uint8_t a = Mask[led];
        if (a == 0)
            continue;
        Buffer[led].Layer(colour, ByteToPercent(a));
    }
}

// Renders the configured render block into the LED buffer, applying brightness and gamma correction.
inline void Vysi1Display::Render()
{
    // Textures apply into the LED buffer ("texture always clears the buffer and
    // applies the texture in the given areas"), so the buffer must be cleared every
    // frame or pixels not covered by the current render would keep stale colours.
    memset((void *)Buffer, 0, LedNum * sizeof(ColourClass));

    if (Data.RenderBlock < 0 || Data.RenderBlock >= dynamic_block_registry.block_count)
        return;
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock((uint16_t)Data.RenderBlock);
    if (!block)
        return;

    // ---- geometry cache (mask): recompute only when the scene actually changed ----
    const Matrix<2, 3> &off = Data.Offset;
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

        uint16_t n = block->map_count < MaxCachedFields ? block->map_count : MaxCachedFields;
        for (uint16_t f = 0; f < n; f++)
            if (BlockMetaType(block->Get(f).Descriptor.FlagsAndType) == (uint16_t)DataType::Geometry)
                RenderGeometryField(block, f);
    }

    // ---- per-frame field pass: build mask sections and fill ----
    memset((void *)Mask, 0, LedNum);
    for (uint16_t f = 0; f < block->map_count; f++)
    {
        uint16_t t = BlockMetaType(block->Get(f).Descriptor.FlagsAndType);
        if (t == (uint16_t)DataType::Geometry && f < MaxCachedFields)
            ApplyGeometryField(block, f);
        else if (t == (uint16_t)DataType::Texture)
        {
            RenderTextureField(block, f);
            memset((void *)Mask, 0, LedNum);
        }
    }

    // Apply Brightness only (the mask/texture alpha is already baked into the RGB by
    // ColourClass::Layer against the cleared buffer; scaling by Buffer.A again would
    // square the alpha so a 50% mask would render at 25%).
    Number brightness = Data.Brightness;
    if (brightness < N(0)) brightness = N(0);
    uint32_t brightness_scale = (brightness >= 100) ? 255 : ((brightness * 255) / 100).ToInt();

    for (uint16_t i = 0; i < LedNum; i++)
    {
        uint32_t r = (Buffer[i].R * brightness_scale) >> 8;
        uint32_t g = (Buffer[i].G * brightness_scale) >> 8;
        uint32_t b = (Buffer[i].B * brightness_scale) >> 8;

        Buffer[i].R = GammaTable[r > 255 ? 255 : r];
        Buffer[i].G = GammaTable[g > 255 ? 255 : g];
        Buffer[i].B = GammaTable[b > 255 ? 255 : b];
    }
}