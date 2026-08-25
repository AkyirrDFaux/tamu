// Gamma = 1.8

#include "Blocks/Render.h"
#include "esp_log.h"

// Converts a fixed-point `Number` to a float.
float NumberToFloat(Number n);

const uint8_t GammaTable[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 4,
    4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7,
    8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11, 12, 12, 12, 13,
    13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21,
    21, 22, 22, 23, 24, 24, 25, 25, 26, 27, 27, 28, 29, 29, 30, 31,
    31, 32, 33, 33, 34, 35, 36, 36, 37, 38, 39, 39, 40, 41, 42, 43,
    43, 44, 45, 46, 47, 47, 48, 49, 50, 51, 52, 53, 53, 54, 55, 56,
    57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72,
    73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 86, 87, 88, 89,
    90, 91, 92, 94, 95, 96, 97, 98, 100, 101, 102, 103, 105, 106, 107, 109,
    110, 111, 113, 114, 115, 117, 118, 119, 121, 122, 123, 125, 126, 128, 129, 131,
    132, 133, 135, 136, 138, 139, 141, 142, 144, 145, 147, 148, 150, 151, 153, 154,
    156, 157, 159, 160, 162, 163, 165, 166, 168, 170, 171, 173, 174, 176, 177, 179,
    181, 182, 184, 185, 187, 188, 190, 192, 193, 195, 196, 198, 199, 200, 200, 200};

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

struct Vysi1Struct
{
    Number Brightness = 30; //%
    Matrix<3, 3> Offset = Matrix<3, 3>::Identity();
    uint32_t RenderBlock = 0;
    // Layout File Name: plain 8-char storage file name, space padded. Default is blank =
    // built-in default layout. Written via trigger, which loads the layout file
    // immediately (write is rejected if the file cannot be loaded).
    char LayoutFile[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    Number RefreshRate;     // Out: achieved render rate in FPS (averaged)
};

const BlockMeta Vysi1_Map[] = {
    {DataType::Number | FieldFlags::None, 0x00, sizeof(Number)},      // Brightness
    {DataType::Matrix | FieldFlags::None, 0x00, sizeof(Matrix<3, 3>)},// Offset
    {DataType::Uint32 | FieldFlags::None, 0x00, sizeof(uint32_t)},    // Render KeyedBlock Index
    {DataType::String | FieldFlags::None, 0x00, 8},                   // Layout File Name
    {DataType::Number | FieldFlags::ReadOnly, 0x00, sizeof(Number)},  // Refresh Rate
};

// Layout-file write trigger: stores the new Name and loads the layout file immediately
// so the stored name always matches the layout in use. `block.Data` is the first member
// of the owning Vysi1Display instance (Main.h registers &DisplayN.Data), which recovers
// the per-display runtime layout.
// The layout-file write trigger (index 3) and the schema are defined after the class:
// the trigger loads through the owning Vysi1Display instance.

class Vysi1Display
{
public:
    // Runtime LED-index table loaded from a layout file (Docs/Modules/LED display.md):
    // row-first W x H uint16 entries, 0xFFFF = missing LED, indexes are 0-based.
    static const uint32_t MaxLayoutEntries = 256;
    static const uint32_t LedNum = 86;

    Vysi1Struct Data;
    uint16_t Layout[MaxLayoutEntries];
    uint8_t Lw = 11;
    uint8_t Lh = 10;
    ColourClass Buffer[LedNum];

    Vysi1Display() { LoadDefaultLayout(); }

    void Render();
    void ResolveGeometryDefinition(KeyedBlockDescriptor *block, uint16_t index, GeometryDefinition &def);
    Number CalculateShapeAlpha(const GeometryDefinition &def, Vector<2> P);
    void RenderGeometry(KeyedBlockDescriptor *block, uint16_t index, const Matrix<3, 3> &BaseTransform, Number *Overlay);
    void ResolveTextureDefinition(KeyedBlockDescriptor *block, uint16_t index, TextureDefinition &def);
    void RenderTexture(KeyedBlockDescriptor *block, uint16_t index, const Matrix<3, 3> &BaseTransform, Number *Overlay);

    // Reverts to the compiled-in default layout (11x10, 0=missing converted to FFFF).
    void LoadDefaultLayout()
    {
        Lw = 11;
        Lh = 10;
        for (uint32_t i = 0; i < MaxLayoutEntries; i++)
            Layout[i] = 0xFFFF;
        for (uint32_t i = 0; i < Lw * Lh && i < MaxLayoutEntries; i++)
            Layout[i] = (LayoutVysiv1_0[i] == 0) ? 0xFFFF : (uint16_t)(LayoutVysiv1_0[i] - 1);
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

const TriggerEntry Vysi1_callbacks[] = {
    {OnVysi1FieldWrite, 3}};

const BlockSchema Vysi1_Schema = {
    .Map = Vysi1_Map,
    .Triggers = Vysi1_callbacks,
    .Type = BlockType::Vysi1Display,
    .MapCount = sizeof(Vysi1_Map) / sizeof(BlockMeta),
    .TriggerCount = sizeof(Vysi1_callbacks) / sizeof(TriggerEntry)};

// Renders the configured render block into the LED buffer, applying brightness and gamma correction.
inline void Vysi1Display::Render()
{
    // Textures apply into the LED buffer ("texture always clears the buffer and
    // applies the texture in the given areas"), so the buffer must be cleared every
    // frame or pixels not covered by the current render would keep stale colours.
    memset((void *)Buffer, 0, LedNum * sizeof(ColourClass));

    Number Overlay[LedNum];
    memset((void *)Overlay, 0, LedNum * sizeof(Number));

    // Example: Center origin, flip Y, and zoom out (scale 0.5)
    Matrix<3, 3> BaseTransform = Data.Offset * Matrix<3, 3>::CreateTransform2D(N(0), {-(N(Lw) / N(2.0) - N(0.5)), -(N(Lh) / N(2.0) - N(0.5))}, {N(1.0), N(1.0)});

    // Shapes and texturesF
    if (Data.RenderBlock >= keyed_block_registry.block_count)
        return;
    KeyedBlockDescriptor *block = keyed_block_registry.GetBlock((uint16_t)Data.RenderBlock);
    if (!block)
        return;

    for (uint16_t i = 0; i < block->map_count; i++)
    {
        if (BlockMetaType(block->Get(i).Descriptor.FlagsAndType) == (uint16_t)DataType::Geometry)
            RenderGeometry(block, i, BaseTransform, Overlay);
        else if (BlockMetaType(block->Get(i).Descriptor.FlagsAndType) == (uint16_t)DataType::Texture)
        {
            RenderTexture(block, i, BaseTransform, Overlay);
            memset((void *)Overlay, 0, LedNum * sizeof(Number));
        }
    }

    // Clamp Brightness into [0, 100]: a negative value would wrap through the
    // uint32_t scale below and produce garbage colours instead of black.
    Number brightness = Data.Brightness;
    if (brightness < N(0)) brightness = N(0);
    uint32_t brightness_scale = (brightness >= 100) ? 255 : ((brightness * 255) / 100).ToInt();

    for (uint16_t i = 0; i < LedNum; i++)
    {
        uint32_t alpha = (uint32_t)Buffer[i].A;
        uint32_t scalar = (brightness_scale * alpha) >> 8;

        uint32_t r = (Buffer[i].R * scalar) >> 8;
        uint32_t g = (Buffer[i].G * scalar) >> 8;
        uint32_t b = (Buffer[i].B * scalar) >> 8;

        Buffer[i].R = GammaTable[r > 255 ? 255 : r];
        Buffer[i].G = GammaTable[g > 255 ? 255 : g];
        Buffer[i].B = GammaTable[b > 255 ? 255 : b];
    }
}

// Fills `def` with the geometry parameters (dimensions, radius, edge fade) stored in the given block entry.
inline void Vysi1Display::ResolveGeometryDefinition(KeyedBlockDescriptor *block, uint16_t index, GeometryDefinition &def)
{
    switch (def.Type)
    {
    case Geometries::Box:
    case Geometries::Elipse:
    case Geometries::Triangle:
        def.Data.Basic.Width = block->GetKeyValue<Number>(index, (uint8_t)GeometryKey::Width, DataType::Number, N(0.0));
        def.Data.Basic.Height = block->GetKeyValue<Number>(index, (uint8_t)GeometryKey::Height, DataType::Number, N(0.0));
        def.Data.Basic.EdgeFade = block->GetKeyValue<Number>(index, (uint8_t)GeometryKey::EdgeFade, DataType::Number, N(0.05));
        break;

    case Geometries::Polygon:
    case Geometries::Star:
        def.Data.Polygon.Radius = block->GetKeyValue<Number>(index, (uint8_t)GeometryKey::Radius, DataType::Number, N(0.0));
        def.Data.Polygon.PointNumber = block->GetKeyValue<Number>(index, (uint8_t)GeometryKey::PointNumber, DataType::Number, N(3.0));
        def.Data.Polygon.EdgeFade = block->GetKeyValue<Number>(index, (uint8_t)GeometryKey::EdgeFade, DataType::Number, N(0.05));
        break;

    case Geometries::HalfFill:
        def.Data.HalfPlane.EdgeFade = block->GetKeyValue<Number>(index, (uint8_t)GeometryKey::EdgeFade, DataType::Number, N(0.05));
        break;

    case Geometries::DoubleParabola:
        def.Data.Basic.Width = block->GetKeyValue<Number>(index, (uint8_t)GeometryKey::Width, DataType::Number, N(0.0));
        def.Data.Basic.Height = block->GetKeyValue<Number>(index, (uint8_t)GeometryKey::Height, DataType::Number, N(0.0));
        def.Data.Basic.EdgeFade = block->GetKeyValue<Number>(index, (uint8_t)GeometryKey::EdgeFade, DataType::Number, N(0.05));
        break;

    default:
        break;
    }
}

// Computes the coverage alpha (0.0-1.0) of the given shape at point `P` in shape-local space.
Number Vysi1Display::CalculateShapeAlpha(const GeometryDefinition &def, Vector<2> P)
{
    Number Distance = 0;
    Number F = N(0.001); // Default fade

    switch (def.Type)
    {
    case Geometries::Box:
    case Geometries::Triangle:
        F = def.Data.Basic.EdgeFade;
        if (def.Type == Geometries::Box)
        {
            Distance = min(def.Data.Basic.Width - abs(P[0]), def.Data.Basic.Height - abs(P[1]));
        }
        else
        {
            // Triangle: 2h|x|/w + y < h
            Distance = min(def.Data.Basic.Height - P[1] - 2 * def.Data.Basic.Height * abs(P[0]) / def.Data.Basic.Width, P[1]);
        }
        break;

    case Geometries::Elipse:
    {
        F = def.Data.Basic.EdgeFade;
        // Bounding box + fade optimization
        if (abs(P[0]) > def.Data.Basic.Width + F || abs(P[1]) > def.Data.Basic.Height + F)
            return 0;
        // Inner full opacity optimization
        if (abs(P[0]) < def.Data.Basic.Width * N(0.7) - F && abs(P[1]) < def.Data.Basic.Height * N(0.7) - F)
            return 1;

        // Local copies: the sq() macro would evaluate each subscript twice
        Number px = P[0], py = P[1];
        Distance = 1 - sqrt(sq(px) / sq(def.Data.Basic.Width) + sq(py) / sq(def.Data.Basic.Height));
        break;
    }

    case Geometries::Star:
    case Geometries::Polygon:
    {
        F = def.Data.Polygon.EdgeFade;
        // Signed distance to a regular n-gon (Polygon) or n-point star, from the
        // angular position of P inside its vertex sector: at angle `ang` off the
        // vertex, the radial extent is v * cos(half) / cos(ang) (v = vertex radius).
        Number R = def.Data.Polygon.Radius;
        if (R <= N(0)) { Distance = N(-1); break; } // unconfigured: draw nothing
        int n = def.Data.Polygon.PointNumber.RoundToInt();
        if (n < 3) n = 3;
        if (n > 32) n = 32;
        const Number n_num = Number(n);

        Number pr = P[0], ppr = P[1];
        Number r = sqrt(sq(pr) + sq(ppr));
        if (r <= N(0)) { Distance = R; break; } // centre is always inside

        // Polar angle of P in [0, 2PI) (atan2 returns -PI..PI).
        Number theta = atan2(ppr, pr);
        if (theta.Value < 0) theta = theta + 2 * GetPI();

        Number sector, half, v;
        if (def.Type == Geometries::Polygon)
        {
            sector = 2 * GetPI() / n_num;
            v = R;
        }
        else
        {
            // Star: 2n vertices alternating outer R / inner R/2.
            sector = 2 * GetPI() / (n_num * N(2.0));
            uint16_t vi = (uint16_t)(theta / sector).ToInt();
            v = (vi & 1) ? R * N(0.5) : R;
        }
        half = sector / N(2.0);
        // Position within the vertex sector, measured from the vertex (cos(ang)
        // is never 0 here: |ang| <= half <= PI/3).
        Number ang = (theta - Number((theta / sector).ToInt()) * sector) - half;
        Distance = (v * cos(half) / cos(ang)) - r;
        break;
    }

    case Geometries::DoubleParabola:
        F = def.Data.Basic.EdgeFade;
        Distance = -(abs(P[0]) - def.Data.Basic.Width + sq(P[1]) * def.Data.Basic.Width / sq(def.Data.Basic.Height));
        break;

    case Geometries::HalfFill:
        F = def.Data.HalfPlane.EdgeFade;
        return LimitZeroToOne(P[1] / F + N(0.5));

    default:
        return 0;
    }

    // Convert Distance to 0.0-1.0 alpha based on EdgeFade
    return LimitZeroToOne(Distance / F + N(0.5));
}

// Rasterises the geometry block entry into the LED `Overlay` buffer, combining transforms and applying the operation.
inline void Vysi1Display::RenderGeometry(KeyedBlockDescriptor *block, uint16_t index, const Matrix<3, 3> &BaseTransform, Number *Overlay)
{
    // 1. Resolve Parameters with Defaults
    GeometryDefinition def;
    def.Type = block->GetKeyValue<Geometries>(index, (uint8_t)GeometryKey::Shape, DataType::Enum, Geometries::None);

    if (def.Type == Geometries::None)
        return;

    GeometryOperation Op = block->GetKeyValue<GeometryOperation>(index, (uint8_t)GeometryKey::Operation, DataType::Enum, GeometryOperation::Add);

    // Fast-path for Fill operations
    if (def.Type == Geometries::Fill)
    {
        for (int32_t i = 0; i < LedNum; i++)
        {
            switch (Op)
            {
            case GeometryOperation::Add:
                Overlay[i] = N(1);
                break;
            case GeometryOperation::Cut:
                Overlay[i] = N(0);
                break;
            case GeometryOperation::Intersect: /* Nothing */
                break;
            case GeometryOperation::XOR:
                Overlay[i] = abs(Overlay[i] - N(1));
                break;
            }
        }
        return;
    }

    // Resolve specific shape properties
    ResolveGeometryDefinition(block, index, def);

    // Fetch Transform: Default to Identity if not explicitly provided
    Matrix<3, 3> LocalTransform = block->GetKeyValue<Matrix<3, 3>>(
        index, (uint8_t)GeometryKey::Transformation, DataType::Matrix, Matrix<3, 3>::Identity());

    // 2. Combine Transforms
    Matrix<3, 3> CombinedTransform = LocalTransform * BaseTransform;

    // 3. Rasterization Loop
    for (int32_t Y = 0; Y < (int32_t)Lh; Y++)
    {
        for (int32_t X = 0; X < (int32_t)Lw; X++)
        {
            uint32_t arrayIdx = ((Lh - 1 - Y) * Lw) + X;
            uint16_t ledIdx = Layout[arrayIdx];

            if (ledIdx == 0xFFFF) // missing LED
                continue;
            uint32_t PIdx = ledIdx;

            Vector<3> homo_pt = {Number(X), Number(Y), Number(1)};
            Vector<3> transformed = CombinedTransform * homo_pt;

            Number LocalAlpha = CalculateShapeAlpha(def, transformed.remove(2));
            if (LocalAlpha <= 0)
                continue;

            // 4. Apply Operation
            switch (Op)
            {
            case GeometryOperation::Add:
                Overlay[PIdx] = LimitZeroToOne(Overlay[PIdx] + LocalAlpha);
                break;
            case GeometryOperation::Cut:
                Overlay[PIdx] = LimitZeroToOne(Overlay[PIdx] - LocalAlpha);
                break;
            case GeometryOperation::Intersect:
                Overlay[PIdx] = LimitZeroToOne(Overlay[PIdx] * LocalAlpha);
                break;
            case GeometryOperation::XOR:
                Overlay[PIdx] = abs(Overlay[PIdx] - LocalAlpha);
                break;
            }
        }
    }
}

// Fills `def` with the texture parameters (colours, blend width) stored in the given block entry.
inline void Vysi1Display::ResolveTextureDefinition(KeyedBlockDescriptor *block, uint16_t index, TextureDefinition &def)
{
    // Fetch Type: Default to None if not found
    def.Type = block->GetKeyValue<Textures2D>(index, (uint8_t)TextureKey::Type, DataType::Enum, Textures2D::None);

    switch (def.Type)
    {
    case Textures2D::Full:
        def.Data.Fill.Colour = block->GetKeyValue<ColourClass>(
            index, (uint8_t)TextureKey::Colour1, DataType::Colour, ColourClass(0, 0, 0, 0));
        break;

    case Textures2D::BlendLinear:
    case Textures2D::BlendCircular:
        def.Data.Blend2.Colour1 = block->GetKeyValue<ColourClass>(
            index, (uint8_t)TextureKey::Colour1, DataType::Colour, ColourClass(0, 0, 0, 0));
        def.Data.Blend2.Colour2 = block->GetKeyValue<ColourClass>(
            index, (uint8_t)TextureKey::Colour2, DataType::Colour, ColourClass(0, 0, 0, 0));
        def.Data.Blend2.Width = block->GetKeyValue<Number>(
            index, (uint8_t)TextureKey::Width, DataType::Number, N(1.0));
        break;

    default:
        break;
    }
}

// Layers the texture block entry's colours onto the LED buffer using the `Overlay` intensities.
inline void Vysi1Display::RenderTexture(KeyedBlockDescriptor *block, uint16_t index, const Matrix<3, 3> &BaseTransform, Number *Overlay)
{
    TextureDefinition def;
    ResolveTextureDefinition(block, index, def);

    if (def.Type == Textures2D::None)
        return;

    // Fetch Transform: Default to Identity
    Matrix<3, 3> LocalTransform = block->GetKeyValue<Matrix<3, 3>>(
        index, (uint8_t)TextureKey::Transformation, DataType::Matrix, Matrix<3, 3>::Identity());

    // Compose the same way as the geometry pass (Local * Base) so textures and their
    // covering geometries are evaluated in the same coordinate space.
    Matrix<3, 3> Combined = LocalTransform * BaseTransform;

    // Handle "Full" texture as a fast path
    if (def.Type == Textures2D::Full)
    {
        for (uint16_t i = 0; i < LedNum; i++)
        {
            if (Overlay[i] <= 0)
                continue;
            Buffer[i].Layer(def.Data.Fill.Colour, Overlay[i]);
        }
        return;
    }

    // Rasterization Loop for Blends
    Number invWidth = N(1.0) / (def.Data.Blend2.Width * N(2.0));

    for (int32_t Y = 0; Y < (int32_t)Lh; Y++)
    {
        for (int32_t X = 0; X < (int32_t)Lw; X++)
        {
            uint32_t arrayIdx = ((Lh - 1 - Y) * Lw) + X;
            uint16_t ledIdx = Layout[arrayIdx];

            if (ledIdx == 0xFFFF) // missing LED
                continue;
            uint32_t PIdx = ledIdx;

            Vector<3> transformed = Combined * Vector<3>{Number(X), Number(Y), N(1)};

            ColourClass blendCol = def.Data.Blend2.Colour2;
            Number lerpVal;

            if (def.Type == Textures2D::BlendLinear)
                lerpVal = (transformed[0] * invWidth) + N(0.5);
            else // BlendCircular
                lerpVal = (transformed.remove(2).norm2() * invWidth) + N(0.5);

            blendCol.Layer(def.Data.Blend2.Colour1, LimitZeroToOne(lerpVal));

            // Apply to buffer with Overlay intensity (skip zero overlap - Layer would
            // recompute all four channels for no effect)
            if (Overlay[PIdx] <= 0)
                continue;
            Buffer[PIdx].Layer(blendCol, Overlay[PIdx]);
        }
    }
}