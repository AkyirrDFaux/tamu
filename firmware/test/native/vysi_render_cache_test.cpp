// Host-side test for the renderer's per-field mask cache (t_a706d64a).
//
// The real Blocks/Vysi1Render.h is compiled unmodified (the same four device-layer stubs as
// vysi_render_kind_test.cpp). What is pinned here is the property the cache exists for: a write
// to one part of a render block must not recompute the other parts' masks, and a write that
// changes nothing must cost nothing - while the frames stay exactly what a full rebuild produces.
//
//   pass A  two renders with no write at all             -> nothing recomputed, frame unchanged
//   pass B  a part's own key rewritten with its own bytes -> its mask is not recomputed
//   pass C  that key changed                             -> exactly that part's mask is recomputed
//   pass D  a part removed / added back                  -> slots move, every mask is rebuilt
//   pass E  a layout load                                -> every mask is rebuilt
//
// "Recomputed" is observed, not timed. RenderGeometryField writes a slot's whole GeoMask, so a
// slot that still holds a sentinel pattern after a render was not rebuilt. Two displays share the
// block: `Observed` gets its masks poisoned and is used only for that observation, `Frames`
// renders normally and provides the frames to compare.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Types/Enums.h"
#include "Core/Types/Number.h"
#include "Core/Types/Vector.h"

#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Packet.h"
DeviceStatusStruct DeviceStatus;
void DispatchPacket(const PacketFrame &) {}
void DeviceLog(const char *, const char *, ...) {}
PacketFrame TxFrame;

#include "Blocks/Vysi1Display.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

static const ValueInfo Enum1 = { (uint16_t)DataType::Enum, 1, 0 };
static const ValueInfo Num4 = { (uint16_t)DataType::Number, 4, 0 };
static const ValueInfo Col4 = { (uint16_t)DataType::Colour, 4, 0 };
static const ValueInfo Mat23 = { (uint16_t)DataType::Matrix, sizeof(Matrix<2, 3>), 0 };
static const ValueInfo Geo0 = { (uint16_t)DataType::Geometry, 0, 0 };
static const ValueInfo Tex0 = { (uint16_t)DataType::Texture, 0, 0 };
static const ValueInfo None0 = { (uint16_t)DataType::None, 0, 0 };

static void Num(DynamicBlockDescriptor &B, uint8_t F, uint8_t K, int32_t Units) {
    Number V = Number::FromRaw(Units * 65536);
    CHECK(B.SetEntry(F, K, &V, 4, Num4));
}

// A Circle part: its size is the piece an emote script moves, like the eye blocks' iris.
static void CirclePart(DynamicBlockDescriptor &B, uint8_t F) {
    uint8_t Shape = (uint8_t)Geometries::Circle;
    uint8_t Op = (uint8_t)GeometryOperation::Replace;
    CHECK(B.SetEntry(F, 0, nullptr, 0, Geo0));
    CHECK(B.SetEntry(F, (uint8_t)GeometryKey::Shape, &Shape, 1, Enum1));
    CHECK(B.SetEntry(F, (uint8_t)GeometryKey::Operation, &Op, 1, Enum1));
    Matrix<2, 3> M;
    M(0, 0) = Number(1); M(0, 1) = Number(0); M(0, 2) = Number(0);
    M(1, 0) = Number(0); M(1, 1) = Number(1); M(1, 2) = Number(0);
    CHECK(B.SetEntry(F, (uint8_t)GeometryKey::Position, &M, sizeof(M), Mat23));
    Num(B, F, (uint8_t)GeometryKey::Size, 6);
}

static void FillPart(DynamicBlockDescriptor &B, uint8_t F) {
    uint8_t Shape = (uint8_t)Geometries::Fill;
    uint8_t Op = (uint8_t)GeometryOperation::Replace;
    CHECK(B.SetEntry(F, 0, nullptr, 0, Geo0));
    CHECK(B.SetEntry(F, (uint8_t)GeometryKey::Shape, &Shape, 1, Enum1));
    CHECK(B.SetEntry(F, (uint8_t)GeometryKey::Operation, &Op, 1, Enum1));
}

static void TexturePart(DynamicBlockDescriptor &B, uint8_t F, uint8_t R, uint8_t G, uint8_t Bl) {
    uint8_t Type = (uint8_t)Textures2D::Fill;
    ColourClass C(R, G, Bl, 255);
    CHECK(B.SetEntry(F, 0, nullptr, 0, Tex0));
    CHECK(B.SetEntry(F, (uint8_t)TextureKey::Type, &Type, 1, Enum1));
    CHECK(B.SetEntry(F, (uint8_t)TextureKey::Colour1, &C, 4, Col4));
}

// Drops every entry of a field (removing just the key-0 marker leaves the values behind, and the
// field would still be listed).
static void DropField(DynamicBlockDescriptor &B, uint8_t F) {
    for (uint8_t K = 0; K <= (uint8_t)GeometryKey::NoiseSeed; K++)
        B.SetEntry(F, K, nullptr, 0, None0);
}

static void PoisonSlots(Vysi1Display &D) {
    memset((void *)D.GeoMask, 0xA5, sizeof(D.GeoMask));
}

static bool SlotPoisoned(const Vysi1Display &D, uint16_t Slot) {
    for (uint16_t i = 0; i < Vysi1Display::LedNum; i++)
        if (D.GeoMask[Slot][i] != 0xA5) return false;
    return true;
}

// ColourClass has a user-defined copy assignment, so frames are copied and compared component by
// component instead of with memcmp on the buffer.
static void SaveFrame(const Vysi1Display &D, ColourClass *Out) {
    for (uint16_t i = 0; i < Vysi1Display::LedNum; i++) Out[i] = D.Buffer[i];
}

static bool BufferEqual(const Vysi1Display &A, const ColourClass *Other) {
    for (uint16_t i = 0; i < Vysi1Display::LedNum; i++) {
        if (A.Buffer[i].Red != Other[i].Red || A.Buffer[i].Green != Other[i].Green ||
            A.Buffer[i].Blue != Other[i].Blue || A.Buffer[i].Alpha != Other[i].Alpha)
            return false;
    }
    return true;
}

static void MapPanel(Vysi1Display &D) {
    for (uint32_t i = 0; i < sizeof(LayoutVysiv1_0); i++)
        D.Layout[i] = (uint16_t)(LayoutVysiv1_0[i] == 0 ? 0xFFFF : LayoutVysiv1_0[i] - 1);
    D.Lw = 11;
    D.Lh = 10;
    D.BrightnessLimit = 178;
    D.RebuildLedTable();
    D.Vol.Brightness = Number(30);
}

int main() {
    Vysi1Volatile VolA, VolB;
    Vysi1Persistent PerA, PerB;
    Vysi1Display Frames(VolA, PerA);
    Vysi1Display Observed(VolB, PerB);
    MapPanel(Frames);
    MapPanel(Observed);

    CHECK(DynamicBlockRegistry.AddBlock());
    PerA.RenderBlock = 0;
    PerB.RenderBlock = 0;
    DynamicBlockDescriptor &Block = *DynamicBlockRegistry.GetBlock(0);

    // f0 Fill (the picture), f1 Circle (the iris), f2 a white fill over both.
    FillPart(Block, 0);
    CirclePart(Block, 1);
    TexturePart(Block, 2, 200, 200, 200);

    // ---- pass A: no write at all.
    Frames.Render();
    Observed.Render();
    ColourClass First[Vysi1Display::LedNum];
    SaveFrame(Frames, First);
    CHECK(Frames.CachedFieldCount == 3);
    CHECK(Frames.CachedFieldKind[0] == 1 && Frames.CachedFieldKind[1] == 1 &&
          Frames.CachedFieldKind[2] == 2);
    CHECK(Frames.CachedFieldOp[0] == (uint8_t)GeometryOperation::Replace);

    PoisonSlots(Observed);
    Observed.Render();
    CHECK(SlotPoisoned(Observed, 0)); // the Fill mask was not recomputed
    CHECK(SlotPoisoned(Observed, 1)); // ... nor the Circle's
    Frames.Render();
    CHECK(BufferEqual(Frames, First));
    std::printf("pass A: no write -> no mask recompute, frame unchanged\n");

    // ---- pass B: the iris's own matrix rewritten with the values it already holds (what a
    // script's per-pass write does while the face is not moving).
    Matrix<2, 3> Same;
    memcpy(&Same, Block.GetKey(1, (uint8_t)GeometryKey::Position).DataPtr, sizeof(Same));
    const uint32_t GenBefore = Block.Generation;
    CHECK(Block.SetEntry(1, (uint8_t)GeometryKey::Position, &Same, sizeof(Same), Mat23));
    CHECK(Block.Generation != GenBefore); // the write did bump the block generation
    PoisonSlots(Observed);
    Observed.Render();
    CHECK(SlotPoisoned(Observed, 0));
    CHECK(SlotPoisoned(Observed, 1)); // an identical rewrite costs no shape pass
    Frames.Render();
    CHECK(BufferEqual(Frames, First));
    std::printf("pass B: identical rewrite -> generation bumped, no mask recompute\n");

    // ---- pass C: the iris shrinks. Only its own slot is rebuilt; the Fill part keeps its mask.
    Num(Block, 1, (uint8_t)GeometryKey::Size, 3);
    PoisonSlots(Observed);
    Observed.Render();
    CHECK(SlotPoisoned(Observed, 0));  // untouched
    CHECK(!SlotPoisoned(Observed, 1)); // the changed part only
    Frames.Render();
    CHECK(!BufferEqual(Frames, First)); // and the frame really did change (a smaller iris)
    ColourClass Moved[Vysi1Display::LedNum];
    SaveFrame(Frames, Moved);
    std::printf("pass C: one part changed -> only that part's mask recomputed\n");

    // ---- pass D: the iris part is removed. The slots shift, so every mask is rebuilt (a stale
    // mask must never be applied to a different field).
    DropField(Block, 1);
    PoisonSlots(Observed);
    Observed.Render();
    CHECK(Observed.CachedFieldCount == 2);
    CHECK(!SlotPoisoned(Observed, 0));

    // Putting it back rebuilds everything again, and the frame converges on the same pixels.
    CirclePart(Block, 1);
    Num(Block, 1, (uint8_t)GeometryKey::Size, 3); // the size pass C left it at
    PoisonSlots(Observed);
    Observed.Render();
    CHECK(Observed.CachedFieldCount == 3);
    CHECK(!SlotPoisoned(Observed, 0));
    CHECK(!SlotPoisoned(Observed, 1));
    Frames.Render();
    CHECK(BufferEqual(Frames, Moved)); // byte for byte the frame from pass C
    std::printf("pass D: a part removed or added -> every mask rebuilt, frames converge\n");

    // ---- pass E: the panel's map changes (a layout-file write). Every mask depends on the shared
    // transform, so all of them are rebuilt.
    Frames.Layout[0] = 0xFFFF; // unplug one LED from the map
    Frames.RebuildLedTable();
    Observed.Layout[0] = 0xFFFF;
    Observed.RebuildLedTable();
    PoisonSlots(Observed);
    Observed.Render();
    CHECK(!SlotPoisoned(Observed, 0));
    CHECK(!SlotPoisoned(Observed, 1));
    std::printf("pass E: layout load -> every mask rebuilt\n");

    Block.Release();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
