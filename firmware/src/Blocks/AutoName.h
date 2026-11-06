#pragma once

// ===== Naming a device after its serial =====
//
// The owner's rule (2026-11-06: "automatically name the devices when they pair if that is
// possible"): the first time a device is bonded it gives itself a name built from the one thing it
// owns that no other device does - its serial number - so a phone shows "Tamu-2070" instead of
// whatever the build happened to ship with.
//
// THE RULE
//
//   "<Prefix>-<HHHH>"
//
//   Prefix   the device's short product name: Tamu / DAS / Valu (AutoNamePrefix)
//   HHHH     the LAST TWO identity bytes of the serial, printed as four UPPERCASE hex digits in
//            the order the System block publishes the serial (field 1), so the visitor can compare
//            the name with the serial shown in the app. Example: the factory MAC
//            E4:B0:63:C8:20:70 gives "Tamu-2070".
//
// The name goes into System `Name` (field 6, a fixed 16-char field, space-padded, no terminator),
// which is the name the BLE advertisement carries while the `BLE Settings` block's Display name is
// empty - i.e. on a fresh device. The rule stays 16-char safe: the longest name it builds is
// "Valu-FFFF" (9).
//
// PRECEDENCE - the auto-name FILLS a name, it never overrides one
//
//   - It applies only to an UNDISTURBED field: empty (every byte a space or NUL), or still exactly
//     the build's factory value (KFactorySystem.Name on the core). Anything else is a name someone
//     chose, and is left alone for good.
//   - A setup restore writes System/Name through the register path, so a restored name is a chosen
//     name: a pairing that happens afterwards does not touch it, and a restore that happens
//     afterwards overwrites the auto-name. The field is the single source of truth - last writer
//     wins, and the auto-name is only ever the FIRST writer.
//   - Because the write leaves a name that is neither empty nor the factory value, the rule fires
//     at most ONCE per device, however many bonds it makes later (or loses and makes again).
//
// WHEN - on the FIRST successful bond: the E66 session gate has already admitted the link
// (encrypted, bonded, and authenticated when a passkey is set) AND the bond store was empty when
// that link came up, so the bond it just made is the device's first. Never on a reconnect: a
// reconnect shows no new bond, and the field is no longer undisturbed anyway. The device half is
// Devices/Tamu_v2.0A/BleAutoName.h.
//
// Host-clean on purpose (no ESP-IDF, no NimBLE, no storage, no register file): the rule itself is
// driven by test/native/auto_name_test.cpp.

#include <cstdint>
#include <cstring>
#include "Core/Types/Enums.h"

// The separator between the prefix and the serial tail, and the tail's shape.
#define AUTO_NAME_SEPARATOR   '-'
#define AUTO_NAME_TAIL_BYTES  2
#define AUTO_NAME_TAIL_DIGITS (AUTO_NAME_TAIL_BYTES * 2)

// The longest name the rule can build ("Valu-FFFF"), so a caller can size a buffer from the rule
// instead of from the current prefixes.
#define AUTO_NAME_MAX_LEN 9

// Whether a Name field byte is padding rather than a character. The System `Name` field is
// space-padded by the build's initialiser and by every write through the register path
// (MemoryTypes.h Set() null-pads a short Text/Name value), so both spellings mean "nothing here".
static inline bool AutoNameIsPadding(char C) { return C == ' ' || C == '\0'; }

// One hex digit, uppercase: the name's tail is compared against the serial the app prints.
static inline char AutoNameHex(uint8_t Value) { return (char)(Value < 10 ? '0' + Value : 'A' + (Value - 10)); }

// The short product name per device type. The default keeps an unknown build unique and short
// rather than leaving it unnamed.
static inline const char *AutoNamePrefix(DeviceType Type)
{
    switch (Type)
    {
    case DeviceType::Tamu_v2_0A:       return "Tamu";
    case DeviceType::DualAnalogSensor: return "DAS";
    case DeviceType::Valu_v2_0:        return "Valu";
    default:                           return "Dev";
    }
}

// Builds the rule's name into `Out` (NUL-terminated) for `Serial`, the `SerialLen` identity bytes
// the device's GetSerialNumber() fills. Returns the name's length, or 0 when it cannot be built:
// no serial tail to take, or no room for the name and its terminator. A caller that gets 0 leaves
// the field alone - a name every device would share is worse than no name at all.
static inline uint8_t AutoNameFormat(char *Out, uint8_t Cap, const char *Prefix,
                                     const uint8_t *Serial, uint8_t SerialLen)
{
    if (!Out || !Prefix || !Serial || Cap == 0 || SerialLen < AUTO_NAME_TAIL_BYTES)
        return 0;

    const uint8_t PrefixLen = (uint8_t)strlen(Prefix);
    const uint8_t Need = (uint8_t)(PrefixLen + 1 + AUTO_NAME_TAIL_DIGITS);
    if ((uint16_t)Need + 1 > Cap)
        return 0;

    uint8_t N = 0;
    memcpy(Out, Prefix, PrefixLen);
    N = PrefixLen;
    Out[N++] = AUTO_NAME_SEPARATOR;
    // The tail is the LAST AUTO_NAME_TAIL_BYTES of the identity, in the order the serial publishes
    // them: the two least significant bytes of the serial number.
    for (uint8_t i = (uint8_t)(SerialLen - AUTO_NAME_TAIL_BYTES); i < SerialLen; i++)
    {
        Out[N++] = AutoNameHex((uint8_t)(Serial[i] >> 4));
        Out[N++] = AutoNameHex((uint8_t)(Serial[i] & 0x0F));
    }
    Out[N] = '\0';
    return N;
}

// Whether a fixed-width Name field has never been given a name of its own: every byte padding, or
// exactly the build's factory value. The factory value's own trailing padding does not have to
// match byte for byte - a field that carries it NUL-padded (what a short register write produces)
// and one that carries it space-padded (the build's initialiser) are the same name.
static inline bool AutoNameUndisturbed(const char *Field, uint8_t Len, const char *Factory, uint8_t FactoryLen)
{
    if (!Field || Len == 0)
        return false;

    if (Factory && FactoryLen && FactoryLen <= Len)
    {
        // The factory value's significant bytes (trailing padding dropped).
        uint8_t Sig = FactoryLen;
        while (Sig > 0 && AutoNameIsPadding(Factory[Sig - 1]))
            Sig--;
        uint8_t i = 0;
        while (i < Sig && Field[i] == Factory[i])
            i++;
        if (i == Sig)
        {
            while (i < Len && AutoNameIsPadding(Field[i]))
                i++;
            if (i == Len)
                return true;
        }
    }

    for (uint8_t i = 0; i < Len; i++)
        if (!AutoNameIsPadding(Field[i]))
            return false;
    return true;
}

// Writes `Name` into a fixed-width Name field, space-padded to `Len`. The field carries no
// terminator - that is its documented shape (Core/Services/StaticMemory.h) - and spaces are what
// SystemNameCString trims, so the name reaches the advertisement as a C string either way.
static inline void AutoNameFieldSet(char *Field, uint8_t Len, const char *Name, uint8_t NameLen)
{
    if (!Field || Len == 0)
        return;
    if (NameLen > Len)
        NameLen = Len;
    memset(Field, ' ', Len);
    if (Name && NameLen)
        memcpy(Field, Name, NameLen);
}

// The trigger, as the rule states it: this link made the device's FIRST bond (`StoreEmptyAtConnect`
// is the bond-store snapshot taken when the link came up, and `BondsNow` is the store the link has
// just added to it) and nobody has named the device yet.
static inline bool AutoNameDue(bool StoreEmptyAtConnect, int BondsNow, bool FieldUndisturbed)
{
    return StoreEmptyAtConnect && BondsNow > 0 && FieldUndisturbed;
}
