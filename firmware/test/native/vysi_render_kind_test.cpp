// Host-side test for the renderer's classification of a render part by its field's key-0 dictionary
// marker (t_2da784f0), and for the marker CREATE that makes such a field reachable at all.
//
// The real Blocks/Vysi1Render.h is compiled unmodified: the render pipeline, the field cache and the
// kind this test reads are the ones that run on the board. Two things have to be supplied for that -
// the block descriptor (Core/Functions/MemoryBlocks.h, already host-clean) and the four device-layer
// symbols the headers merely reference but the render path never calls (DeviceStatus, TxFrame,
// DispatchPacket, DeviceLog). With those stubbed there is no packet, storage or GPIO dependency, so
// the whole scene runs on the host.
//
// The scene mirrors the measured rig case (tamu-setup archive fc69c0c9, "Left Eye"): a picture
// (geometry + grey fill) and an appended overlay (geometry + black fill) at fields 9 and 10.
//   pass A - the overlay's two key-0 rows were created by an EMPTY host write (a Geometry/Texture
//            ValueInfo with Size 0 and no value bytes) - the shape a setup restore now sends;
//   pass B - those two rows deleted, everything else untouched: the state the card measured as the
//            rig symptom (fields still listed, classified 0, never applied, picture shows through).
// The existing fields 0/1 must read exactly the same in both passes: a block whose key-0 rows
// already exist behaves as before.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Types/Enums.h"
#include "Core/Types/Number.h"
#include "Core/Types/Vector.h"

// The device-layer symbols the block headers reference. None of them is reached from the render path.
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

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
                        #got, #want, g, w);                                          \
        }                                                                            \
    } while (0)

static const ValueInfo Enum1 = { (uint16_t)DataType::Enum, 1, 0 };
static const ValueInfo Col4 = { (uint16_t)DataType::Colour, 4, 0 };
static const ValueInfo Geo0 = { (uint16_t)DataType::Geometry, 0, 0 };
static const ValueInfo Tex0 = { (uint16_t)DataType::Texture, 0, 0 };
static const ValueInfo None = { (uint16_t)DataType::None, 0, 0 };

// A geometry part: Shape = Fill (255 alpha over every present LED, independent of the transform).
static void GeometryPart(DynamicBlockDescriptor &B, uint8_t Field) {
    uint8_t Shape = (uint8_t)Geometries::Fill;
    CHECK(B.SetEntry(Field, (uint8_t)GeometryKey::Shape, &Shape, 1, Enum1));
}

// A texture part: Type = Fill in one colour.
static void TexturePart(DynamicBlockDescriptor &B, uint8_t Field, ColourClass Colour) {
    uint8_t Type = (uint8_t)Textures2D::Fill;
    CHECK(B.SetEntry(Field, (uint8_t)TextureKey::Type, &Type, 1, Enum1));
    CHECK(B.SetEntry(Field, (uint8_t)TextureKey::Colour1, &Colour, 4, Col4));
}

int main() {
    Vysi1Volatile Vol;
    Vysi1Persistent Per;
    Vysi1Display Disp(Vol, Per);

    // One LED on the panel: cell 0 carries it, so the fill pass has somewhere to write.
    Disp.Lw = 1;
    Disp.Lh = 1;
    Disp.Layout[0] = 1;
    Disp.LedPresent[0] = true;
    Disp.LedX[0] = 0;
    Disp.LedY[0] = 0;
    Disp.LedCount = 1;
    Disp.BrightnessLimit = 255; // the layout's cap: Brightness is used unscaled
    Disp.LayoutGen++;
    Vol.Brightness = 100;

    CHECK(DynamicBlockRegistry.AddBlock());
    Per.RenderBlock = 0;
    DynamicBlockDescriptor &Parts = *DynamicBlockRegistry.GetBlock(0);

    // The picture and the appended overlay. Every key-0 row here is created by an EMPTY write, which
    // is the operation this card added: before it, none of these four rows could exist.
    CHECK(Parts.SetEntry(0, 0, nullptr, 0, Geo0));
    GeometryPart(Parts, 0);
    CHECK(Parts.SetEntry(1, 0, nullptr, 0, Tex0));
    TexturePart(Parts, 1, ColourClass(178, 178, 178, 255)); // the picture: grey
    CHECK(Parts.SetEntry(9, 0, nullptr, 0, Geo0));
    GeometryPart(Parts, 9);
    CHECK(Parts.SetEntry(10, 0, nullptr, 0, Tex0));
    TexturePart(Parts, 10, ColourClass(0, 0, 0, 255)); // the overlay: black

    // The marker rows really are markers: the declared type, no value, no data pointer.
    KeyResult M9 = Parts.GetKey(9, 0);
    KeyResult M10 = Parts.GetKey(10, 0);
    CHECK_EQ(ValueInfoType(M9.Meta.Type), (uint16_t)DataType::Geometry);
    CHECK_EQ(ValueInfoType(M10.Meta.Type), (uint16_t)DataType::Texture);
    CHECK_EQ(M9.Meta.Size, 0);
    CHECK_EQ(M10.Meta.Size, 0);
    CHECK(M9.DataPtr == nullptr);
    CHECK(M10.DataPtr == nullptr);

    // ---- pass A: with the markers, the renderer classifies and applies both parts.
    Disp.Render();
    std::printf("pass A: %u field(s) ->", (unsigned)Disp.CachedFieldCount);
    for (uint16_t i = 0; i < Disp.CachedFieldCount; i++)
        std::printf(" field %u kind %u;", (unsigned)Disp.CachedFields[i], (unsigned)Disp.CachedFieldKind[i]);
    std::printf(" LED0 (%u,%u,%u,%u)\n", (unsigned)Disp.Buffer[0].Red, (unsigned)Disp.Buffer[0].Green,
                (unsigned)Disp.Buffer[0].Blue, (unsigned)Disp.Buffer[0].Alpha);
    CHECK_EQ(Disp.CachedFieldCount, 4);
    CHECK(Disp.CachedFields[0] == 0 && Disp.CachedFieldKind[0] == 1);   // geometry
    CHECK(Disp.CachedFields[1] == 1 && Disp.CachedFieldKind[1] == 2);   // texture
    CHECK(Disp.CachedFields[2] == 9 && Disp.CachedFieldKind[2] == 1);   // the created marker
    CHECK(Disp.CachedFields[3] == 10 && Disp.CachedFieldKind[3] == 2);  // ... and its texture
    const ColourClass WithMarkers = Disp.Buffer[0];
    CHECK(WithMarkers.Red == 0 && WithMarkers.Green == 0 && WithMarkers.Blue == 0);
    CHECK_EQ(WithMarkers.Alpha, 255); // the black overlay covers the picture

    // ---- pass B: the two key-0 rows deleted, nothing else touched.
    CHECK(Parts.SetEntry(9, 0, nullptr, 0, None));
    CHECK(Parts.SetEntry(10, 0, nullptr, 0, None));
    CHECK(!Parts.GetKey(9, 0).Exists);
    Disp.Render();
    std::printf("pass B: %u field(s) ->", (unsigned)Disp.CachedFieldCount);
    for (uint16_t i = 0; i < Disp.CachedFieldCount; i++)
        std::printf(" field %u kind %u;", (unsigned)Disp.CachedFields[i], (unsigned)Disp.CachedFieldKind[i]);
    std::printf(" LED0 (%u,%u,%u,%u)\n", (unsigned)Disp.Buffer[0].Red, (unsigned)Disp.Buffer[0].Green,
                (unsigned)Disp.Buffer[0].Blue, (unsigned)Disp.Buffer[0].Alpha);
    CHECK_EQ(Disp.CachedFieldCount, 4);   // the fields are still listed (the values are still there)
    CHECK(Disp.CachedFields[2] == 9 && Disp.CachedFieldKind[2] == 0);   // ... but unclassified
    CHECK(Disp.CachedFields[3] == 10 && Disp.CachedFieldKind[3] == 0);
    CHECK(Disp.Buffer[0].Red != 0);       // the picture shows through instead of the overlay
    std::printf("LED0 with markers (%u,%u,%u,%u), without (%u,%u,%u,%u)\n", (unsigned)WithMarkers.Red,
                (unsigned)WithMarkers.Green, (unsigned)WithMarkers.Blue, (unsigned)WithMarkers.Alpha,
                (unsigned)Disp.Buffer[0].Red, (unsigned)Disp.Buffer[0].Green,
                (unsigned)Disp.Buffer[0].Blue, (unsigned)Disp.Buffer[0].Alpha);

    // ---- a block whose key-0 rows already exist behaves exactly as before: fields 0 and 1 kept
    // their kind and their rendering through both passes, and re-sending an existing marker changes
    // nothing (a restore is idempotent).
    CHECK(Disp.CachedFieldKind[0] == 1);
    CHECK(Disp.CachedFieldKind[1] == 2);
    const uint16_t Entries = Parts.EntryCount;
    CHECK(Parts.SetEntry(0, 0, nullptr, 0, Geo0));
    CHECK_EQ(Parts.EntryCount, Entries);
    CHECK_EQ(Parts.GetKey(0, 0).Meta.Size, 0);

    Parts.Release();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
