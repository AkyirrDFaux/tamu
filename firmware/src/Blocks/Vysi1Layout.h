#pragma once

// Block structs, the display class and the layout/boot/field-write hooks.
//
// Part of the LED display block; included by
// Blocks/Vysi1Display.h in dependency order.

#include "Blocks/Vysi1Gamma.h"
#include "Blocks/GeometryMath.h" // GeometryParams/PrepareGeometry/ShapeAlpha
#include "Blocks/Render.h"
#include "Blocks/Vysi1LayoutFile.h" // layout-file decode (shared with the host tests)
#include "Core/Functions/Memory.h"
#include "Core/Types/Matrix.h"
#include "Core/Types/Vector.h"

#include <cstddef> // offsetof

// The compiled-in layout: 11x10 cells, 1-based LED indexes (0 = a cell with no LED). constexpr so
// the capacity check below can read it at compile time.
constexpr uint8_t LayoutVysiv1_0[10 * 11]{
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

// Preloads the Vysi v1.0 LED layout into storage as "LAY_1" on first boot. Only created
// when absent, so a user's customized layout is never overwritten. File format per
// Docs/Modules/LED display.md: u8 brightness limit (0-255 as a percentage), u8 width, u8
// height, then w*h u16 LE 0-based LED indices (0xFFFF = unused).
inline void PreloadVysiLayout()
{
    char Lay1[8];
    PackName("LAY_1", Lay1);
    if (Storage.FileExists(Lay1) != 0xFFFFFFFF)
        return; // already present (preloaded earlier)

    uint8_t Buf[3 + 11 * 10 * 2];
    Buf[0] = 178; // brightness limit: 0-255 as a percentage -> 70%
    Buf[1] = 11;  // width
    Buf[2] = 10;  // height
    // The 3-byte header puts every u16 at an odd offset, so write them through memcpy.
    for (uint32_t i = 0; i < 11 * 10; i++)
        StoreUnaligned(Buf + 3 + i * 2,
                       (uint16_t)((LayoutVysiv1_0[i] == 0) ? 0xFFFF : (LayoutVysiv1_0[i] - 1)));

    if (!Storage.CreateFile(Lay1, sizeof(Buf)))
        return;
    if (!Storage.WriteToFile(Lay1, 0, sizeof(Buf), (const char *)Buf))
    {
        // A half-written LAY_1 must not survive: the FileExists guard above would then skip it
        // on every later boot and the display would run on a corrupt layout forever. Delete it
        // so the next boot retries the preload.
        Storage.DeleteFile(Lay1);
        return;
    }
}

// Identity 2x3 affine ([1 0 0; 0 1 0]), the default Offset transform.
static inline Matrix<2, 3> IdentityAffine23()
{
    Matrix<2, 3> Mat;
    Mat(0, 0) = Number(1);
    Mat(0, 1) = Number(0);
    Mat(0, 2) = Number(0);
    Mat(1, 0) = Number(0);
    Mat(1, 1) = Number(1);
    Mat(1, 2) = Number(0);
    return Mat;
}

struct Vysi1Volatile
{
    Number Brightness = 30; //% (offset 0)
    Number RefreshRate;     // Out: achieved render rate in FPS (averaged), offset 4
};
struct Vysi1Persistent
{
    Matrix<2, 3> Offset = IdentityAffine23(); // 2x3 transformation, offset 0 (28 B: u16 height + u16 width + 6 Numbers)
    int32_t RenderBlock = -1; // Signed index into DynamicBlockRegistry; -1 = none, offset 28
    // Layout File Name: plain 8-char storage file name, space padded. Defaults to the
    // preloaded "LAY_1" file (identical to the compiled-in layout); a blank name means
    // the built-in default layout. Written via trigger, which loads the layout file
    // immediately (write is rejected if the file cannot be loaded).
    char LayoutFile[8] = {'L', 'A', 'Y', '_', '1', ' ', ' ', ' '}; // offset 32
};

// The dynamic field offsets in Vysi1_Entries must track the struct layout: the 2x3
// Offset Matrix occupies 28 B (4-byte header + 6 Numbers), so RenderBlock is at 28
// and LayoutFile at 32. These asserts fail the build if either drifts.
static_assert(offsetof(Vysi1Persistent, RenderBlock) == 28, "RenderBlock must be at offset 28");
static_assert(offsetof(Vysi1Persistent, LayoutFile) == 32, "LayoutFile must be at offset 32");

const BlockEntry Vysi1_Entries[] = {
    { MakeFieldKey(0, 0), 0,  {(uint16_t)DataType::Number, sizeof(Number), 0} },                     // Brightness
    { MakeFieldKey(1, 0), 0,  {(uint16_t)DataType::Matrix, sizeof(Matrix<2, 3>), ValuePersistent} }, // Offset (2x3)
    { MakeFieldKey(2, 0), 28, {(uint16_t)DataType::Index, sizeof(int32_t), ValuePersistent} },       // Render Block Index (signed, -1 = none)
    { MakeFieldKey(3, 0), 32, {(uint16_t)DataType::Filename, 8, ValueTrigger | ValuePersistent} },   // Layout File Name
    { MakeFieldKey(4, 0), 4,  {(uint16_t)DataType::Number, sizeof(Number), ValueReadOnly} },         // Refresh Rate
};

// Renders the configured render block into the LED buffer, applying brightness and gamma correction.
// Performance-oriented rewrite: geometry contributions are cached per block generation
// (mask cache), so an unchanged scene costs one buffer clear + a per-LED fill pass per frame.
class Vysi1Display
{
public:
    static const uint32_t MaxLayoutEntries = 256;
    // Chain capacity: the most LEDs this board can drive, i.e. the buffers below hold indices
    // 0…86. It is NOT "the panel's LED count" - that comes from the layout file (LedCount), so a
    // file addressing 86 LEDs and one addressing 87 load on the same image. This constant only
    // bounds the buffers and the loader's index check.
    static const uint32_t LedNum = 87;
    // Maximum render "parts" (geometry/texture fields) cached per block; 12 leaves headroom
    // for richer scenes.
    static const uint32_t MaxCachedFields = 12;

    // The block's two halves live in the board's static spaces; the display holds references.
    Vysi1Volatile &Vol;
    Vysi1Persistent &Per;

    // The field-write trigger is handed only the block, so the displays self-register to let it
    // recover the owning instance from a block's persistent half.
    static const uint32_t MaxInstances = 4;
    static Vysi1Display *Instances[MaxInstances];
    static uint32_t InstanceCount;

    uint16_t Layout[MaxLayoutEntries];
    uint8_t Lw = 11;
    uint8_t Lh = 10;
    // Docs/Modules and blocks/LED display.md: the layout file's leading byte, a 0-255
    // percentage cap on Vol.Brightness (178 = 70%). Replaced whenever a layout file loads;
    // the built-in default carries the same value as the preloaded LAY_1.
    uint8_t BrightnessLimit = 178;
    ColourClass Buffer[LedNum];
    // How many of those pixels this panel's layout actually uses: the highest LED index the loaded
    // layout carries, + 1. The per-panel send walks `LedCount` pixels, so the count follows the
    // layout file rather than a firmware figure (Vysi1LayoutFile::Count).
    uint16_t LedCount = 0;

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
    // MaxCachedFields x LedNum bytes per display (~1 KB at the 87-LED capacity); two displays cost
    // 2 KB of static RAM.
    uint8_t GeoMask[MaxCachedFields][LedNum];
    DynamicBlockDescriptor *CacheBlockPtr = nullptr;
    uint32_t CacheBlockGen = 0xFFFFFFFF;
    int32_t CacheOffset[6] = {0, 0, 0, 0, 0, 0};
    uint32_t CacheLayoutGen = 0xFFFFFFFF;
    bool CacheValid = false;

    // Per-frame field classification, cached alongside the geometry mask and recomputed
    // only when the block/offset/layout changes. Kind: 1 = geometry, 2 = texture, 0 = other.
    uint8_t CachedFields[MaxCachedFields];
    uint8_t CachedFieldKind[MaxCachedFields];
    uint16_t CachedFieldCount = 0;

    // Set by ApplyGeometryField when any geometry wrote a non-zero alpha this frame.
    // Textures/effects apply only inside the mask, so an empty mask skips them entirely.
    bool MaskAny = false;

    Vysi1Display(Vysi1Volatile &Vol, Vysi1Persistent &Per) : Vol(Vol), Per(Per) {
        if (InstanceCount < MaxInstances) Instances[InstanceCount++] = this;
        LoadDefaultLayout();
    }

    // Resolves the owning display from a block's persistent half (the field-write trigger only
    // receives the block, not the instance).
    static Vysi1Display *FromPersistent(const void *Per) {
        for (uint32_t i = 0; i < InstanceCount; i++)
            if ((const void *)&Instances[i]->Per == Per) return Instances[i];
        return nullptr;
    }

    void Render();
    void RenderGeometryField(DynamicBlockDescriptor *Block, uint16_t Field, uint16_t Slot);
    void ApplyGeometryField(DynamicBlockDescriptor *Block, uint16_t Field, uint16_t Slot);
    void RenderTextureField(DynamicBlockDescriptor *Block, uint16_t Field);
    Matrix<3, 3> PromoteAffine(const Matrix<2, 3> &Mat);
    Matrix<3, 3> BaseTransform();
    Matrix<3, 3> FieldTransform(const Matrix<2, 3> &Pos);
    int32_t ReadKeyInt(DynamicBlockDescriptor *Block, uint16_t Field, uint8_t Key, int32_t Def);
    ColourClass LerpColour(const ColourClass &C1, const ColourClass &C2, Number Factor);
    void ShiftHue(ColourClass &Colour, Number HueDeg);

    // Rebuilds the per-LED screen-coordinate table from the current layout, recomputes the panel's
    // LED count and bumps LayoutGen (invalidates the geometry cache).
    void RebuildLedTable()
    {
        for (uint32_t i = 0; i < LedNum; i++)
        {
            LedPresent[i] = false;
            LedX[i] = 0;
            LedY[i] = 0;
        }
        uint32_t Used = (uint32_t)Lw * Lh;
        if (Used > MaxLayoutEntries)
            Used = MaxLayoutEntries;
        for (uint32_t Index = 0; Index < Used; Index++)
        {
            uint16_t Idx = Layout[Index];
            // The loader rejects an index beyond the chain before committing, so this guard only
            // covers the compiled-in table and any future writer of Layout[].
            if (Idx == 0xFFFF || Idx >= LedNum)
                continue;
            LedX[Idx] = (uint8_t)(Index % Lw);
            LedY[Idx] = (uint8_t)(Lh - 1 - (Index / Lw)); // screen y, 0 at bottom
            LedPresent[Idx] = true;
        }
        // The panel's LED count is whatever the cells in use address: highest index + 1.
        LedCount = Vysi1LayoutFile::Count(Layout, Used);
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

    // Loads the layout file named by Per.LayoutFile (8-char storage name form). The file's cells
    // decide how many LEDs the panel has (LedCount); the only limit the firmware adds is the chain
    // capacity, and a cell beyond it rejects the whole file instead of blanking that pixel.
    Vysi1LayoutFile::Result LoadLayoutFromStorage()
    {
        // Blank name (all spaces or NULs; character fields are NUL-padded since B10) ->
        // built-in default layout.
        bool Empty = true;
        for (int i = 0; i < 8; i++)
            if (Per.LayoutFile[i] != ' ' && Per.LayoutFile[i] != '\0') Empty = false;
        if (Empty)
        {
            LoadDefaultLayout();
            return Vysi1LayoutFile::Result::Ok;
        }

        uint32_t Off, Size;
        if (!Storage.GetFileInfo(Per.LayoutFile, &Off, &Size))
            return Vysi1LayoutFile::Result::Missing;

        // Docs/Modules and blocks/LED display.md: u8 brightness limit, u8 width, u8 height,
        // then the index table. Only this format is accepted - a file written to the older
        // 2-byte header fails here and the caller falls back to the built-in default.
        uint32_t Entries = 0;
        uint8_t Hdr[3];
        Vysi1LayoutFile::Result Res;
        if (Size < 3)
            Res = Vysi1LayoutFile::Result::TooSmall;
        else if (StorageFlash.Read(Off, Hdr, 3) != 3)
            Res = Vysi1LayoutFile::Result::ReadFailed;
        else
            Res = Vysi1LayoutFile::ParseHeader(Hdr, Size, MaxLayoutEntries, Entries);
        if (Res != Vysi1LayoutFile::Result::Ok)
            return Res;

        // Read into a scratch buffer and only commit to the display once the whole read
        // succeeded, so a failed read leaves the previous layout/state untouched.
        uint16_t Pending[MaxLayoutEntries];
        if (StorageFlash.Read(Off + 3, (void *)Pending, Entries * 2) != Entries * 2)
            return Vysi1LayoutFile::Result::ReadFailed;
        Res = Vysi1LayoutFile::ParseTable(Pending, Entries, (uint16_t)LedNum);
        if (Res != Vysi1LayoutFile::Result::Ok)
            return Res;

        for (uint32_t i = 0; i < Entries; i++)
            Layout[i] = Pending[i];
        BrightnessLimit = Hdr[0];
        Lw = Hdr[1];
        Lh = Hdr[2];
        RebuildLedTable(); // recomputes LedCount from the committed cells
        return Vysi1LayoutFile::Result::Ok;
    }
};

// The LED count is the layout file's, so these guard the two things a file cannot move: the
// per-LED buffers are capacity-sized from LedNum (one number in one place), and the compiled-in
// layout table must fit that capacity. Both fail the build rather than the panel.
static_assert(sizeof(Vysi1Display::Buffer) == Vysi1Display::LedNum * sizeof(ColourClass),
              "Buffer must hold one ColourClass per LED of the chain capacity");
static_assert(sizeof(Vysi1Display::Mask) == Vysi1Display::LedNum, "Mask must be one byte per LED");
static_assert(sizeof(Vysi1Display::LedX) == Vysi1Display::LedNum, "LedX must be one byte per LED");
static_assert(sizeof(Vysi1Display::LedY) == Vysi1Display::LedNum, "LedY must be one byte per LED");
static_assert(sizeof(Vysi1Display::LedPresent) == Vysi1Display::LedNum, "LedPresent must be one flag per LED");
static_assert(sizeof(Vysi1Display::GeoMask) == Vysi1Display::MaxCachedFields * Vysi1Display::LedNum,
              "GeoMask must be one byte per LED per cached field");

// Highest 1-based index the compiled-in table uses (0 = a cell with no LED).
constexpr uint16_t MaxBuiltinLayoutIndex()
{
    uint16_t Max = 0;
    for (uint32_t i = 0; i < sizeof(LayoutVysiv1_0); i++)
        if (LayoutVysiv1_0[i] > Max)
            Max = LayoutVysiv1_0[i];
    return Max;
}
static_assert(MaxBuiltinLayoutIndex() >= 1 && MaxBuiltinLayoutIndex() <= Vysi1Display::LedNum,
              "the compiled-in layout must address LEDs that fit the chain capacity");

inline Vysi1Display *Vysi1Display::Instances[Vysi1Display::MaxInstances] = {};
inline uint32_t Vysi1Display::InstanceCount = 0;

// Boot helper: loads the named layout file (blank name = compiled-in default). The boot
// recall restores the LayoutFile RAM field but does not re-run its write trigger, so the
// layout must be re-applied explicitly.
inline void Vysi1BootLayout(Vysi1Display &Disp)
{
    // A file that cannot be loaded (missing, truncated, over-capacity, or an older 2-byte-header
    // format) must not leave the display on its zero-initialised Layout[] - every cell there maps
    // to LED 0 - so fall back to the built-in default. The refusal itself is named
    // (Vysi1LayoutFile::ResultName) for whoever logs it.
    if (Disp.LoadLayoutFromStorage() != Vysi1LayoutFile::Result::Ok)
        Disp.LoadDefaultLayout();
}

// Layout-file write trigger: stores the new Name and loads the layout file immediately
// so the stored name always matches the layout in use. The owning Vysi1Display is recovered
// from the block's persistent half, which recovers the per-display runtime layout.
inline bool OnVysi1FieldWrite(const StaticBlockDescriptor& Block, uint16_t Index, const void* Data, uint16_t Len)
{
    if (Index != 3 || Len != 8)
        return false;
    Vysi1Display* Disp = Vysi1Display::FromPersistent(Block.PersistentData);
    if (!Disp)
        return false;
    // Validate against the NEW name before committing it, so a failed layout
    // load leaves the stored name matching the layout actually in use
    // ("stored value equals applied value"). LoadLayoutFromStorage reads
    // Per.LayoutFile, so load through a temporary and only copy on success.
    char Pending[8];
    memcpy(Pending, Data, 8);
    char Saved[8];
    memcpy(Saved, Disp->Per.LayoutFile, 8);
    memcpy(Disp->Per.LayoutFile, Pending, 8);
    bool Ok = Disp->LoadLayoutFromStorage() == Vysi1LayoutFile::Result::Ok;
    if (!Ok)
        memcpy(Disp->Per.LayoutFile, Saved, 8); // revert the RAM field
    return Ok;
}

const BlockTrigger Vysi1_Triggers[] = {
    { MakeFieldKey(3, 0), OnVysi1FieldWrite },
};

const BlockSchema Vysi1Schema = {
    .Entries = Vysi1_Entries,
    .EntryCount = sizeof(Vysi1_Entries) / sizeof(BlockEntry),
    .Triggers = Vysi1_Triggers,
    .TriggerCount = sizeof(Vysi1_Triggers) / sizeof(BlockTrigger),
    .Type = BlockType::Vysi1Display,
};

