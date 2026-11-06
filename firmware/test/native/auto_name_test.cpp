// Host-side test for the naming rule (Blocks/AutoName.h): "Tamu-2070" out of a serial, and the
// precedence that keeps the rule from ever overwriting a name somebody chose.
//
// The rule (owner, 2026-11-06): the first time a device is bonded it names itself
// "<Prefix>-<HHHH>", HHHH being the last two identity bytes of its serial - and ONLY while the
// System `Name` field is undisturbed (empty, or still the build's factory value). A name written by
// the app or by a setup restore is a chosen name: it wins, before or after the pairing.
//
// The trigger itself (first bond, and the write) is device code (Devices/Tamu_v2.0A/BleAutoName.h);
// what is pinned here is the rule, the field shape it writes, and the decision table.

#include <cstdio>
#include <cstring>

#include "Blocks/AutoName.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                            \
    } while (0)

// The Tamu's serial: the low 48 bits of the factory MAC, in printed order
// (Main.h GetSerialNumber, E4:B0:63:C8:20:70).
static const uint8_t TamuSerial[14] = {0xE4, 0xB0, 0x63, 0xC8, 0x20, 0x70, 0, 0, 0, 0, 0, 0, 0, 0};
// A DAS: the CH32V003 chip ID read as a uint32 and memcpy'd little-endian (Devices/DAS_v0.1/Main.h
// GetSerialNumber), i.e. chip ID 0x1A2BA264 -> bytes 64 A2 2B 1A.
static const uint8_t DasSerial[14] = {0x64, 0xA2, 0x2B, 0x1A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// The build's factory name (Tamu_v2.0A/Main.h KFactorySystem.Name) - 16 chars, space-padded.
static const char Factory[16] = {'T', 'a', 'm', 'u', ' ', 'v', '2', '.', '0', 'A', ' ',
                                 ' ', ' ', ' ', ' ', ' '};

// The trim SystemNameCString applies to the fixed 16-char field (trailing spaces dropped), which is
// how the field becomes the C string the advertisement carries.
static void FieldToCString(const char Field[16], char Out[17])
{
    int Len = 16;
    while (Len > 0 && Field[Len - 1] == ' ')
        Len--;
    memcpy(Out, Field, (size_t)Len);
    Out[Len] = '\0';
}

int main()
{
    printf("### naming rule: a device names itself after its serial on its first bond\n");

    // ---- the rule: "<Prefix>-<last two identity bytes, uppercase hex>" ---------------------
    {
        char Name[AUTO_NAME_MAX_LEN + 1] = {0};
        const uint8_t Len = AutoNameFormat(Name, sizeof(Name), AutoNamePrefix(DeviceType::Tamu_v2_0A),
                                          TamuSerial, 6);
        CHECK(Len == 9);
        CHECK(strcmp(Name, "Tamu-2070") == 0);
        // The name is the app's own spelling of the serial's tail, so a person can match the two.
        CHECK(Name[5] == '2' && Name[6] == '0' && Name[7] == '7' && Name[8] == '0');
        CHECK(Name[Len] == '\0');
    }

    // Prefix per product, and the tail of ANY identity length is its last two bytes.
    CHECK(strcmp(AutoNamePrefix(DeviceType::Tamu_v2_0A), "Tamu") == 0);
    CHECK(strcmp(AutoNamePrefix(DeviceType::DualAnalogSensor), "DAS") == 0);
    CHECK(strcmp(AutoNamePrefix(DeviceType::Valu_v2_0), "Valu") == 0);
    CHECK(strcmp(AutoNamePrefix(DeviceType::Unknown), "Dev") == 0); // still unique, still short
    {
        // The DAS's four identity bytes: the tail is bytes 2..3 as PUBLISHED (little-endian chip
        // ID), so chip ID 0x1A2BA264 gives "DAS-2B1A". The "DAS-A264" spelling would need the
        // chip ID's big-endian value, which the serial's bytes do not carry - the rule follows the
        // bytes, so a person comparing the name with System field 1 reads the same digits.
        char Name[AUTO_NAME_MAX_LEN + 1] = {0};
        const uint8_t Len = AutoNameFormat(Name, sizeof(Name), AutoNamePrefix(DeviceType::DualAnalogSensor),
                                          DasSerial, 4);
        CHECK(Len == 8);
        CHECK(strcmp(Name, "DAS-2B1A") == 0);
    }

    // A 1-byte identity has no tail: every device would share one name, so nothing is built.
    {
        char Name[AUTO_NAME_MAX_LEN + 1] = {0};
        CHECK(AutoNameFormat(Name, sizeof(Name), "Tamu", TamuSerial, 1) == 0);
        CHECK(AutoNameFormat(Name, sizeof(Name), "Tamu", TamuSerial, 0) == 0);
        CHECK(AutoNameFormat(Name, sizeof(Name), nullptr, TamuSerial, 6) == 0);
        CHECK(AutoNameFormat(nullptr, sizeof(Name), "Tamu", TamuSerial, 6) == 0);
        CHECK(AutoNameFormat(Name, sizeof(Name), "Tamu", nullptr, 6) == 0);
    }

    // A buffer that cannot hold the name plus its terminator builds nothing (never a truncated
    // name, which could collide with another device's).
    {
        char Exact[AUTO_NAME_MAX_LEN];     // 9: room for the name, none for the terminator
        char Fits[AUTO_NAME_MAX_LEN + 1];  // 10: the smallest buffer that works
        CHECK(AutoNameFormat(Exact, sizeof(Exact), "Tamu", TamuSerial, 6) == 0);
        CHECK(AutoNameFormat(Fits, sizeof(Fits), "Tamu", TamuSerial, 6) == 9);
        CHECK(AutoNameFormat(Fits, 5, "Tamu", TamuSerial, 6) == 0);
    }

    // ---- "undisturbed": the field holds no name of its own ---------------------------------
    CHECK(AutoNameUndisturbed(Factory, 16, Factory, 16));   // the build's factory value
    {
        char Empty[16];
        memset(Empty, ' ', sizeof(Empty));
        CHECK(AutoNameUndisturbed(Empty, 16, Factory, 16)); // all spaces (a blank field)
        memset(Empty, 0x00, sizeof(Empty));
        CHECK(AutoNameUndisturbed(Empty, 16, Factory, 16)); // all NULs (a cleared field)

        // The factory value with the padding a NUL-padding writer leaves: still undisturbed.
        char FactoryNul[16];
        memcpy(FactoryNul, Factory, 10);
        memset(FactoryNul + 10, 0x00, 6);
        CHECK(AutoNameUndisturbed(FactoryNul, 16, Factory, 16));

        // A chosen name, however short, is never disturbed - the app's own write null-pads.
        char Chosen[16];
        memset(Chosen, 0x00, sizeof(Chosen));
        memcpy(Chosen, "Kitchen", 7);
        CHECK(!AutoNameUndisturbed(Chosen, 16, Factory, 16));

        // The auto-name itself: after the write the field must NOT read as undisturbed, or the
        // rule would fire again on the next bond (a rename on every re-pair).
        char Named[16];
        AutoNameFieldSet(Named, 16, "Tamu-2070", 9);
        CHECK(!AutoNameUndisturbed(Named, 16, Factory, 16));

        // A name that shares the factory prefix but continues differently is a chosen name.
        char PrefixOnly[16];
        memcpy(PrefixOnly, Factory, 16);
        PrefixOnly[9] = 'B'; // "Tamu v2.0B"
        CHECK(!AutoNameUndisturbed(PrefixOnly, 16, Factory, 16));
    }

    // ---- the field shape the write produces ------------------------------------------------
    {
        char Field[16];
        memset(Field, '?', sizeof(Field)); // prove every byte is written, not just the name
        AutoNameFieldSet(Field, 16, "Tamu-2070", 9);
        CHECK(memcmp(Field, "Tamu-2070", 9) == 0);
        for (int i = 9; i < 16; i++)
            CHECK(Field[i] == ' '); // space-padded, no terminator (the field's documented shape)

        char Text[17];
        FieldToCString(Field, Text);
        CHECK(strcmp(Text, "Tamu-2070") == 0);

        // A name at the full width fills the field with no padding at all.
        AutoNameFieldSet(Field, 16, "0123456789ABCDEF", 16);
        CHECK(memcmp(Field, "0123456789ABCDEF", 16) == 0);
        // A longer name is clamped rather than overrunning the field.
        AutoNameFieldSet(Field, 16, "0123456789ABCDEFGHI", 19);
        CHECK(memcmp(Field, "0123456789ABCDEF", 16) == 0);
    }

    // ---- the trigger: first bond, and a name nobody has set --------------------------------
    CHECK(AutoNameDue(/*StoreEmptyAtConnect=*/true, /*BondsNow=*/1, /*Undisturbed=*/true));
    CHECK(!AutoNameDue(true, 0, true));   // no bond any more (forgotten before the write ran)
    CHECK(!AutoNameDue(false, 2, true));  // a reconnect, or a later bond: not the device's first
    CHECK(!AutoNameDue(true, 1, false));  // somebody has named it

    // ---- the whole table, as a device lives it ---------------------------------------------
    {
        char Field[16];
        memcpy(Field, Factory, 16); // a build nobody has named

        // 1. First bond: a store that came up empty, one bond now, field undisturbed -> name it.
        CHECK(AutoNameDue(true, 1, AutoNameUndisturbed(Field, 16, Factory, 16)));
        char Name[AUTO_NAME_MAX_LEN + 1];
        const uint8_t Len = AutoNameFormat(Name, sizeof(Name), "Tamu", TamuSerial, 6);
        AutoNameFieldSet(Field, 16, Name, Len);
        char Text[17];
        FieldToCString(Field, Text);
        CHECK(strcmp(Text, "Tamu-2070") == 0);

        // 2. The bonds are erased and the device pairs again: the store is empty at connect and
        //    holds one bond again, but the field now carries the auto-name - no second rename.
        CHECK(!AutoNameDue(true, 1, AutoNameUndisturbed(Field, 16, Factory, 16)));
        CHECK(strcmp(Text, "Tamu-2070") == 0);

        // 3. A setup restore names it "Kitchen" (the register write, then the field's own `.SV`
        //    save). A later pairing leaves it alone: the restore wins after the auto-name...
        AutoNameFieldSet(Field, 16, "Kitchen", 7);
        CHECK(!AutoNameDue(true, 1, AutoNameUndisturbed(Field, 16, Factory, 16)));
        FieldToCString(Field, Text);
        CHECK(strcmp(Text, "Kitchen") == 0);

        // ...and the restore also wins BEFORE any auto-name: a device whose restore landed first
        // never gets an auto-name at all, because its field is no longer undisturbed.
        char Restored[16];
        AutoNameFieldSet(Restored, 16, "Kitchen", 7);
        CHECK(!AutoNameDue(true, 1, AutoNameUndisturbed(Restored, 16, Factory, 16)));
        FieldToCString(Restored, Text);
        CHECK(strcmp(Text, "Kitchen") == 0);
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
