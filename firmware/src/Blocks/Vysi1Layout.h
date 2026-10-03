#pragma once

// Block structs, the display class and the layout/boot/field-write hooks.
//
// Part of the LED display block; included by
// Blocks/Vysi1Display.h in dependency order.

#include "Blocks/Vysi1Gamma.h"
#include "Blocks/GeometryMath.h" // GeometryParams/PrepareGeometry/ShapeAlpha
#include "Blocks/Render.h"
#include "Core/Functions/Memory.h"
#include "Core/Types/Matrix.h"
#include "Core/Types/Vector.h"

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

// Preloads the Vysi v1.0 LED layout into storage as "LAY_1" on first boot (the
// project's layouts/ directory holds the same bytes). Only created when absent, so a
// user's customized layout is never overwritten. File format per Docs/Modules/LED
// display.md: u8 brightness limit (0-255 as a percentage), u8 width, u8 height, then
// w*h u16 LE 0-based LED indices (0xFFFF = unused).
inline void PreloadVysiLayout()
{
    // Heal older preloads: an earlier 5x5 grid ("LAY5X5") and the first build's
    // "VYSIV1 \0" name (a NUL inside the 8-byte record, unreachable through the
    // space-padded lookups).
    char name5[8];
    PackName("LAY5X5", name5);
    Storage.DeleteFile(name5);
    const char legacyNul[8] = {'V', 'Y', 'S', 'I', 'V', '1', ' ', '\0'};
    Storage.DeleteFileExact(legacyNul);

    // Adopt the previous Vysi layout file as LAY_1 (keeps a user's customization).
    char lay1[8];
    PackName("LAY_1", lay1);
    if (Storage.FileExists(lay1) == 0xFFFFFFFF)
    {
        char legacy[8];
        PackName("VYSIV1", legacy);
        if (Storage.FileExists(legacy) != 0xFFFFFFFF)
            Storage.RenameFile(legacy, lay1);
    }
    if (Storage.FileExists(lay1) != 0xFFFFFFFF)
        return; // already present (renamed or preloaded earlier)

    uint8_t buf[3 + 11 * 10 * 2];
    buf[0] = 178; // brightness limit: 0-255 as a percentage -> 70%
    buf[1] = 11;  // width
    buf[2] = 10;  // height
    // The 3-byte header puts every u16 at an odd offset, so write them through memcpy.
    for (uint32_t i = 0; i < 11 * 10; i++)
        StoreUnaligned(buf + 3 + i * 2,
                       (uint16_t)((LayoutVysiv1_0[i] == 0) ? 0xFFFF : (LayoutVysiv1_0[i] - 1)));

    if (!Storage.CreateFile(lay1, sizeof(buf)))
        return;
    Storage.WriteToFile(lay1, 0, sizeof(buf), (const char *)buf);
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

struct Vysi1Volatile
{
    Number Brightness = 30; //% (offset 0)
    Number RefreshRate;     // Out: achieved render rate in FPS (averaged), offset 4
};
struct Vysi1Persistent
{
    Matrix<2, 3> Offset = IdentityAffine23(); // 2x3 transformation, offset 0
    int32_t RenderBlock = -1; // Signed index into dynamic_block_registry; -1 = none, offset 24
    // Layout File Name: plain 8-char storage file name, space padded. Defaults to the
    // preloaded "LAY_1" file (identical to the compiled-in layout); a blank name means
    // the built-in default layout. Written via trigger, which loads the layout file
    // immediately (write is rejected if the file cannot be loaded).
    char LayoutFile[8] = {'L', 'A', 'Y', '_', '1', ' ', ' ', ' '}; // offset 28
};

const ValueInfo Vysi1_Map[] = {
    {(uint16_t)DataType::Number, sizeof(Number), 0},                    // Brightness
    {(uint16_t)DataType::Matrix, sizeof(Matrix<2, 3>), ValuePersistent},// Offset (2x3)
    {(uint16_t)DataType::Index, sizeof(int32_t), ValuePersistent},      // Render Block Index (signed, -1 = none)
    {(uint16_t)DataType::Filename, 8, ValueTrigger | ValuePersistent},  // Layout File Name
    {(uint16_t)DataType::Number, sizeof(Number), ValueReadOnly},        // Refresh Rate
};

// Renders the configured render block into the LED buffer, applying brightness and gamma correction.
// Performance-oriented rewrite: geometry contributions are cached per block generation
// (mask cache), so an unchanged scene costs one buffer clear + a per-LED fill pass per frame.
class Vysi1Display
{
public:
    static const uint32_t MaxLayoutEntries = 256;
    static const uint32_t LedNum = 86;
    // Maximum render "parts" (geometry/texture fields) cached per block; 12 leaves headroom
    // for richer scenes.
    static const uint32_t MaxCachedFields = 12;

    // The block's two halves live in the board's static spaces; the display holds references.
    Vysi1Volatile &Vol;
    Vysi1Persistent &Per;

    // The field-write trigger is handed only the block, so the displays self-register to let it
    // recover the owning instance from a block's persistent half.
    static const uint32_t MaxInstances = 4;
    static Vysi1Display *s_instances[MaxInstances];
    static uint32_t s_instanceCount;

    uint16_t Layout[MaxLayoutEntries];
    uint8_t Lw = 11;
    uint8_t Lh = 10;
    // Docs/Modules and blocks/LED display.md: the layout file's leading byte, a 0-255
    // percentage cap on Vol.Brightness (178 = 70%). Replaced whenever a layout file loads;
    // the built-in default carries the same value as the preloaded LAY_1.
    uint8_t BrightnessLimit = 178;
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

    Vysi1Display(Vysi1Volatile &vol, Vysi1Persistent &per) : Vol(vol), Per(per) {
        if (s_instanceCount < MaxInstances) s_instances[s_instanceCount++] = this;
        LoadDefaultLayout();
    }

    // Resolves the owning display from a block's persistent half (the field-write trigger only
    // receives the block, not the instance).
    static Vysi1Display *FromPersistent(const void *per) {
        for (uint32_t i = 0; i < s_instanceCount; i++)
            if ((const void *)&s_instances[i]->Per == per) return s_instances[i];
        return nullptr;
    }

    void Render();
    void RenderGeometryField(DynamicBlockDescriptor *block, uint16_t field, uint16_t slot);
    void ApplyGeometryField(DynamicBlockDescriptor *block, uint16_t field, uint16_t slot);
    void RenderTextureField(DynamicBlockDescriptor *block, uint16_t field);
    Matrix<3, 3> PromoteAffine(const Matrix<2, 3> &m);
    Matrix<2, 3> IdentityAffine();
    Matrix<3, 3> BaseTransform();
    int32_t ReadKeyInt(DynamicBlockDescriptor *block, uint16_t field, uint8_t key, int32_t def);
    ColourClass LerpColour(const ColourClass &c1, const ColourClass &c2, Number t);
    void ShiftHue(ColourClass &c, Number hueDeg);

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
        BrightnessLimit = 178; // keep the built-in default's cap (70%)
        for (uint32_t i = 0; i < MaxLayoutEntries; i++)
            Layout[i] = 0xFFFF;
        for (uint32_t i = 0; i < Lw * Lh && i < MaxLayoutEntries; i++)
            Layout[i] = (LayoutVysiv1_0[i] == 0) ? 0xFFFF : (uint16_t)(LayoutVysiv1_0[i] - 1);
        RebuildLedTable();
    }

    // Loads the layout file named by Per.LayoutFile (8-char storage name form).
    // Rejects files that are malformed or whose LED indexes exceed this display.
    bool LoadLayoutFromStorage()
    {
        // Blank name (all spaces) -> built-in default layout.
        bool empty = true;
        for (int i = 0; i < 8; i++)
            if (Per.LayoutFile[i] != ' ' && Per.LayoutFile[i] != '\0') empty = false;
        if (empty)
        {
            LoadDefaultLayout();
            return true;
        }

        char n8[8];
        PackName(Per.LayoutFile, n8); // normalize to space-padded form
        uint32_t off, size;
        if (!Storage.GetFileInfo(n8, &off, &size))
            return false;
        if (size < 3)
            return false;

        // Docs/Modules and blocks/LED display.md: u8 brightness limit, u8 width, u8 height,
        // then the index table. Only this format is accepted - a file written to the older
        // 2-byte header fails here and the caller falls back to the built-in default.
        uint8_t hdr[3];
        Storage_FlashRead(off, hdr, 3);
        uint32_t entries = (uint32_t)hdr[1] * hdr[2];
        if (hdr[1] == 0 || hdr[2] == 0 || entries > MaxLayoutEntries ||
            size < 3 + entries * 2)
            return false;

        Storage_FlashRead(off + 3, (void *)Layout, entries * 2);
        for (uint32_t i = 0; i < entries; i++)
            if (Layout[i] != 0xFFFF && Layout[i] >= LedNum)
                Layout[i] = 0xFFFF; // index beyond this display's chain
        BrightnessLimit = hdr[0];
        Lw = hdr[1];
        Lh = hdr[2];
        RebuildLedTable();
        return true;
    }
};

inline Vysi1Display *Vysi1Display::s_instances[Vysi1Display::MaxInstances] = {};
inline uint32_t Vysi1Display::s_instanceCount = 0;

// Boot helper: migrates a persisted old-format "VYSIV1" reference to the renamed
// "LAY_1", then loads the named layout file (blank name = compiled-in default). The
// boot recall restores the LayoutFile RAM field but does not re-run its write trigger,
// so the layout must be re-applied explicitly.
inline void Vysi1BootLayout(Vysi1Display &disp)
{
    char legacy[8];
    PackName("VYSIV1", legacy);
    if (memcmp(disp.Per.LayoutFile, legacy, 8) == 0)
        PackName("LAY_1", disp.Per.LayoutFile);
    // A file that cannot be loaded (missing, truncated, or an older 2-byte-header format)
    // must not leave the display on its zero-initialised Layout[] - every cell there maps to
    // LED 0 - so fall back to the built-in default, whose LED mapping matches the .lay file.
    if (!disp.LoadLayoutFromStorage())
        disp.LoadDefaultLayout();
}

// Layout-file write trigger: stores the new Name and loads the layout file immediately
// so the stored name always matches the layout in use. The owning Vysi1Display is recovered
// from the block's persistent half, which recovers the per-display runtime layout.
inline bool OnVysi1FieldWrite(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t len)
{
    if (index != 3 || len != 8)
        return false;
    Vysi1Display* disp = Vysi1Display::FromPersistent(block.PersistentData);
    if (!disp)
        return false;
    // Validate against the NEW name before committing it, so a failed layout
    // load leaves the stored name matching the layout actually in use
    // ("stored value equals applied value"). LoadLayoutFromStorage reads
    // Per.LayoutFile, so load through a temporary and only copy on success.
    char pending[8];
    memcpy(pending, data, 8);
    char saved[8];
    memcpy(saved, disp->Per.LayoutFile, 8);
    memcpy(disp->Per.LayoutFile, pending, 8);
    bool ok = disp->LoadLayoutFromStorage();
    if (!ok)
        memcpy(disp->Per.LayoutFile, saved, 8); // revert the RAM field
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
    0,   // Brightness (volatile, Number, 4B)
    0,   // Offset (persistent, Matrix<2,3>, 24B)
    24,  // RenderBlock (persistent, int32, 4B)
    28,  // LayoutFile (persistent, String, 8B)
    4,   // RefreshRate (volatile, Number, 4B)
};

const BlockSchema Vysi1_Schema = {
    .Map = Vysi1_Map,
    .Triggers = Vysi1_Triggers,
    .Offsets = Vysi1_Offsets,
    .Type = BlockType::Vysi1Display,
    .MapCount = sizeof(Vysi1_Map) / sizeof(ValueInfo),
    .VolatileSize = 8,
    .PersistentSize = 36,
};

