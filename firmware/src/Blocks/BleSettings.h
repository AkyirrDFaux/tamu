#pragma once

// ===== BLE Settings =====
// The BLE link's settings and state as register fields, so it is visible and settable through the
// register path the app already speaks, instead of a side-channel command set. Block type 0x0B,
// one instance. The radio half (pairing, encrypted attributes, the connectable window, the bond
// store) lives in Devices/Tamu_v2.0A/AppBLE.h; this block is the single source of truth for the
// settings and a read-out of what the radio is doing.
//
// Six fields, and nothing else (owner ruling 2026-10-29, "simplify the BLE block"):
//
//   Enabled         (0, 0)    Bool, Persistent       false = the radio is not used at all
//   Passkey         (1, 0)    Text(6), Trigger       the code the phone types; "000000" = Just Works
//   Mode            (2, 0)    Enum, RO               effective policy, derived from Passkey
//   Display name    (3, 0)    Text(16), Persistent   advertised name, empty = System.Name
//   Bonds           (4, 0)    Undefined, Trigger     the bond store: N x (address + peer name)
//   Window          (5, 0)    Uint32, Trigger        ms left in the connectable window
//
// Field numbers run in the order of that table (the owner's table lists the keys, not the field
// indexes; this is the one place they are fixed). Writes are restricted - see the triggers and the
// change hook in Devices/Tamu_v2.0A/BleSettingsGlue.h: every write to this block is refused unless
// the frame arrived over USB (Core/Functions/AppInterface.h publishes the link). A bonded phone
// that can rotate the key, wipe the bonds, rename the device or switch its radio off is exactly
// what the passkey exists to stop, so renaming needs USB.
//
// `Display name` IS the advertised name. There is no second read-out of the resolved name any more:
// a write re-advertises at once (AppBLESettingsApply rebuilds the advertisement in the same loop
// pass), so the field and the air cannot disagree. "Applied at the next advertising start" is not
// good enough on a device that advertises whenever it is idle - that could mean never.
//
// `Window` is a command and a read-out at the same time: it reads the milliseconds LEFT in the
// connectable (pairing) window, and a write sets the deadline (write the ms you want, 0 closes it).
// The `Action` enum that used to do this - and to forget every bond - is gone; forgetting is now
// zeroing the `Bonds` array, which is the same array the read-out comes from.
//
// ===== The Bonds array =====
// One `Undefined` value of BLE_BOND_ARRAY_LEN bytes: BLE_SETTINGS_BOND_SLOTS fixed-size entries,
// each `address(6) + peer name(16)` (BLE_BOND_ENTRY_LEN). The field's declared size sets how many
// entries there are, so a host reads the whole list in one read and writes it in one write (the
// register path requires the declared size for a non-Text field, so a partial write is refused by
// the dispatcher before any trigger runs).
//
//   - The address is the peer's identity address, published in the order it is printed
//     (AA:BB:CC:DD:EE:FF -> bytes 0..5), NOT NimBLE's least-significant-first internal order.
//   - An ALL-ZERO address is an empty slot. It is what an empty slot reads as, and writing one over
//     a slot deletes that bond.
//   - The name is a label kept beside the address: the NimBLE bond store keeps no peer name
//     (Issues.md E60), so the name is ours, keyed by the address and stored in NVS
//     (Devices/Tamu_v2.0A/BleBondLabelStore.h). It is what the host writes into an entry, so the
//     owner can tell two bonded phones apart; it is not part of the pairing.
//
// What a write means, and the boundary it cannot cross:
//   - an entry's NAME is writable for any slot that holds a bond;
//   - zeroing an entry's ADDRESS deletes that bond (the peer has to pair again inside a window);
//   - a NON-ZERO address in a slot that holds no bond - or holds a different one - is REFUSED as a
//     whole write. A bond cannot be created by writing a register: the keys (LTK/IRK) live in the
//     store, never in the register file, and a fabricated entry would claim a peer that can never
//     authenticate. The reply is FLAG_FAIL with no payload (RegisterWrite.h RespondStatus(false))
//     and nothing in the frame is applied - names included.
//
// Persistence, and why the passkey is the exception:
//   - Enabled and Display name are Persistent fields: the register machinery mirrors them, with the
//     rest of the static persistent space, to `.SV` in the `storage` (SPIFFS) partition.
//   - The Passkey is NOT a persistent register field. `.SV` is a file in the storage partition and
//     the Storage service streams files out by name, so a persistent field is readable by anything
//     that can speak the Storage service - not a place for a secret. The value lives in NVS
//     (namespace `ble`, key `passkey`; Devices/Tamu_v2.0A/BlePasskeyStore.h) and the register field
//     is served from there: the read-out is the live RAM value the store loaded at boot and every
//     accepted write goes to NVS before it is applied. It is readable (this guards a local radio
//     link, and the app should be able to show the user their own code) but never persisted here.
//   - Bonds and Window are volatile: the array is re-derived from the bond store (plus the peer-name
//     store) on every pass that needs it, so it cannot drift from what the radio holds.
//   - The retired first 6 bytes of the persistent struct are kept only so an existing `.SV` keeps
//     its layout; they are scrubbed to `000000` at boot and the mirror rewritten if an older build
//     left a real key there (BlePasskeyInit).
// A firmware reflash keeps both `.SV` and the NVS `ble` namespace; a full erase, a partition-table
// change or a storage `Format()` wipes `.SV` (Enabled, Display name) while NVS keeps the passkey
// and the peer names. The Bonds array is not stored anywhere of its own: it is a view.

#include <cstddef>
#include <cstdint>
#include "Core/Functions/MemoryTypes.h"
#include "Blocks/BleSecurityPolicy.h"

// Bond slots: one per CONFIG_BT_NIMBLE_MAX_BONDS (3). The array exposes that many entries; a device
// with more bonds than slots would silently hide the extras, so the two numbers are pinned together
// in BleSettingsGlue.h.
#define BLE_SETTINGS_BOND_SLOTS 3
// One entry: the peer's identity address, then its name. The address is six bytes, printed order.
// The name is 16 characters (the width of every name field on this device, and of the advertised
// name), NUL-padded, with no terminator guaranteed - a host that writes 16 characters fills it.
// BLE_BOND_ENTRY_LEN and BLE_BOND_ARRAY_LEN are literals so the field's declared size is one number
// a reader (and the app's contract test) can parse; the static_asserts below tie them to the parts.
#define BLE_BOND_ADDR_LEN 6
#define BLE_BOND_NAME_LEN 16
#define BLE_BOND_ENTRY_LEN 22
#define BLE_BOND_ARRAY_LEN 66
static_assert(BLE_BOND_ENTRY_LEN == BLE_BOND_ADDR_LEN + BLE_BOND_NAME_LEN, "bond entry is address + name");
static_assert(BLE_BOND_ARRAY_LEN == BLE_SETTINGS_BOND_SLOTS * BLE_BOND_ENTRY_LEN,
              "the Bonds field's size must be exactly MAX_BONDS x entry");
// The whole array has to travel in one register frame: BlockInfo(4) + ValueInfo(4) + value, and the
// payload is MAX_PAYLOAD_SIZE. A wider entry would make the write path silently truncate it.
static_assert(BLE_BOND_ARRAY_LEN <= 100, "the Bonds value must fit one register frame (MAX_PAYLOAD_SIZE - 8)");

// The advertised name field. BLE advertising carries 31 bytes of data and the name shares them
// with the service UUID, so 16 (the System Name's own width) is the sane ceiling.
#define BLE_DISPLAY_NAME_LEN 16

// Offsets into the two value spaces (see StaticBlockDescriptor::Base).
#define BLE_SET_FIELD_ENABLED      0
#define BLE_SET_FIELD_PASSKEY      1
#define BLE_SET_FIELD_MODE         2
#define BLE_SET_FIELD_DISPLAYNAME  3
#define BLE_SET_FIELD_BONDS        4
#define BLE_SET_FIELD_WINDOW       5

#define BLE_SET_MODE_JUST_WORKS 0
#define BLE_SET_MODE_PASSKEY    1

// When the block's Bonds array must be re-read from the bond store (used by the board glue and
// pinned by test/native/ble_settings_test.cpp).
//
// The store's SIZE is not a change signal: when a full store takes a fourth bond, the stack evicts
// the oldest entry and adds the new one inside one callback, so getNumBonds() reads the same
// number before and after while the entries underneath are different. The read-out is therefore
// keyed on the store's CONTENT - a generation counter the radio code bumps on every completed bond
// and on a forget - with the count and a slow re-format-and-compare as the safety net for a store
// that changes without either event.
struct BleBondRefresh
{
    uint32_t Generation = 0; // store generation the array was last formatted from
    int      Count = 0;      // getNumBonds() at that moment
    uint32_t DueMs = 0;      // TimeFromBoot() at which the slow content check is due
};

static inline bool BleBondRefreshDue(const BleBondRefresh &State, uint32_t Generation, int Count,
                                     uint32_t NowMs)
{
    // DueMs starts at 0, so the first pass after boot always formats; the signed difference keeps
    // the 49-day TimeFromBoot() wrap from stalling the check.
    return Generation != State.Generation || Count != State.Count ||
           (int32_t)(NowMs - State.DueMs) >= 0;
}

// Whether a Bonds entry's address is the empty/invalid one (all zero). Six zero bytes mean "no
// bond in this slot" - never a peer, because a real identity address is not all zero.
static inline bool BleBondAddrEmpty(const uint8_t Addr[BLE_BOND_ADDR_LEN])
{
    for (uint8_t i = 0; i < BLE_BOND_ADDR_LEN; i++)
        if (Addr[i] != 0)
            return false;
    return true;
}

struct BleSettingsPersistent
{
    // Retired: layout only. This is where the passkey used to be persisted; the layout is kept so
    // an existing `.SV` still decodes (Enabled at 6, Display name at 8) and the bytes are scrubbed
    // to "000000" at boot. Never read as the passkey - see BleSettingsVolatile::Passkey.
    char    ScrubbedPasskey[BLE_PASSKEY_LEN] = {'0', '0', '0', '0', '0', '0'}; // field 0, offset 0
    // Default on: a device nobody has configured is still reachable over BLE (and the pairing
    // window decides when), so a fresh frame is usable from the phone alone.
    uint8_t Enabled = 1;                                              // field 0, offset 6
    // alignas(4): a value is aligned to its size capped at 4 (Docs/Services/Register.md "Memory
    // with 32-bit alignment"), so the 16-byte name sits at 8 and not at 7.
    alignas(4) char DisplayName[BLE_DISPLAY_NAME_LEN] = {};            // field 3, offset 8
};

struct BleSettingsVolatile
{
    // The live passkey text: served from the NVS-backed store, written through to it by the
    // trigger. Not persistent, so it never reaches `.SV` (see the header note).
    char     Passkey[BLE_PASSKEY_LEN] = {'0', '0', '0', '0', '0', '0'}; // field 1, offset 0
    uint8_t  Mode      = BLE_SET_MODE_JUST_WORKS;                   // field 2, offset 6
    uint8_t  Reserved  = 0;                                         // offset 7: Bonds stays 4-aligned
    // The bond list, re-derived from the store: BLE_SETTINGS_BOND_SLOTS entries of
    // "identity address (6, printed order) + peer name (16, NUL-padded)". All-zero address = empty.
    alignas(4) uint8_t Bonds[BLE_BOND_ARRAY_LEN] = {};               // field 4, offsets 8..73
    // Milliseconds LEFT in the connectable window, 0 when it is closed. A write sets the deadline.
    uint32_t WindowMs  = 0;                                         // field 5, offset 76
};

// The entry table addresses the value spaces by byte offset, so the structs and the offsets above
// must agree. Pinned here rather than discovered as a read that returns the wrong field.
static_assert(offsetof(BleSettingsPersistent, ScrubbedPasskey) == 0, "retired passkey offset");
static_assert(offsetof(BleSettingsPersistent, Enabled) == 6, "Enabled offset");
static_assert(offsetof(BleSettingsPersistent, DisplayName) == 8, "Display name offset");
static_assert(offsetof(BleSettingsVolatile, Passkey) == 0, "Passkey offset");
static_assert(offsetof(BleSettingsVolatile, Mode) == 6, "Mode offset");
static_assert(offsetof(BleSettingsVolatile, Bonds) == 8, "Bonds offset");
static_assert(offsetof(BleSettingsVolatile, WindowMs) == 76, "Window offset");
// A value is copied from the base of its space, and the register docs require 32-bit alignment.
static_assert((offsetof(BleSettingsVolatile, Bonds) % 4) == 0, "Bonds must be 4-byte aligned");

// Defined in the board glue: the write side of the block (Devices/Tamu_v2.0A/BleSettingsGlue.h).
bool OnBleEnabledWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnBlePasskeyWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnBleDisplayNameWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnBleBondsWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnBleWindowWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);

const BlockEntry BleSettings_Entries[] = {
    // Settable settings. Both Persistent and both USB-only: Enabled has a trigger as well as the
    // copy path because the whole block is closed to the radio, not just its secrets.
    { MakeFieldKey(BLE_SET_FIELD_ENABLED, 0), 6,
      { (uint16_t)DataType::Bool, sizeof(uint8_t), (uint8_t)(ValuePersistent | ValueTrigger) } },
    // The passkey text: readable (the app may show the user their own code) and writable, but NOT
    // persistent - the value is kept in NVS by the trigger and served from the volatile space.
    { MakeFieldKey(BLE_SET_FIELD_PASSKEY, 0), 0,
      { (uint16_t)DataType::Text, BLE_PASSKEY_LEN, (uint8_t)ValueTrigger } },
    // Read-only report of the policy the passkey implies.
    { MakeFieldKey(BLE_SET_FIELD_MODE, 0), 6,
      { (uint16_t)DataType::Enum, 1, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_DISPLAYNAME, 0), 8,
      { (uint16_t)DataType::Text, BLE_DISPLAY_NAME_LEN, (uint8_t)(ValuePersistent | ValueTrigger) } },
    // The bond list: one Undefined value covering every slot (see the header). Editable - an entry's
    // name may be written and a zeroed address deletes that bond - but it can never create one.
    { MakeFieldKey(BLE_SET_FIELD_BONDS, 0), 8,
      { (uint16_t)DataType::Undefined, BLE_BOND_ARRAY_LEN, (uint8_t)ValueTrigger } },
    // The pairing window: reads the ms left, a write sets the deadline (0 closes it).
    { MakeFieldKey(BLE_SET_FIELD_WINDOW, 0), 76,
      { (uint16_t)DataType::Uint32, sizeof(uint32_t), (uint8_t)ValueTrigger } },
};

const BlockTrigger BleSettings_Triggers[] = {
    { MakeFieldKey(BLE_SET_FIELD_ENABLED, 0), OnBleEnabledWrite },
    { MakeFieldKey(BLE_SET_FIELD_PASSKEY, 0), OnBlePasskeyWrite },
    { MakeFieldKey(BLE_SET_FIELD_DISPLAYNAME, 0), OnBleDisplayNameWrite },
    { MakeFieldKey(BLE_SET_FIELD_BONDS, 0), OnBleBondsWrite },
    { MakeFieldKey(BLE_SET_FIELD_WINDOW, 0), OnBleWindowWrite },
};

const BlockSchema BleSettingsSchema = {
    .Entries = BleSettings_Entries,
    .EntryCount = sizeof(BleSettings_Entries) / sizeof(BlockEntry),
    .Triggers = BleSettings_Triggers,
    .TriggerCount = sizeof(BleSettings_Triggers) / sizeof(BlockTrigger),
    .Type = BlockType::BleSettings,
};
