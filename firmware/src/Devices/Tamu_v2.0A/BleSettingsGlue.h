#pragma once

// The device half of the `BLE Settings` block (Blocks/BleSettings.h): the write triggers, the boot
// load of the passkey store and the per-loop hook that keeps the block's read-only fields level
// with the radio. Included from Main.h AFTER AppBLE.h, because it drives the link.
//
// Who may change the block: EVERY write to it is refused unless the frame arrived over the USB
// link. The source is not inferred from which interfaces happen to be up - AppInterface publishes
// the link a frame arrived on while it is dispatched (Core/Functions/AppInterface.h,
// AppFrameCameOverUsb), so a write from a bonded, authenticated phone is refused with a normal
// FLAG_FAIL reply the app can show. A bonded phone must not be able to rotate the key, wipe the
// bonds, rename the device or switch its radio off; renaming therefore needs USB.

#include "Blocks/BleSettings.h"
#include "BlePasskeyStore.h"
#include "BleBondLabelStore.h" // the peer names the Bonds array carries (NVS, keyed by address)

static_assert(BLE_SETTINGS_BOND_SLOTS == CONFIG_BT_NIMBLE_MAX_BONDS,
              "the Bonds array must expose exactly the bond store's capacity");

// The block's one write rule. `AppFrameCameOverUsb` is true only for a frame that arrived on the
// USB link: a BLE frame, a bus frame and a locally generated write all fail it.
static bool BleBlockWritable()
{
    return AppFrameCameOverUsb();
}

// Boot: prime the passkey from NVS and purge any secret an older build persisted in `.SV`.
//
// The passkey used to be a Persistent register field. That put it in `.SV`, in the storage
// partition, where the Storage service will stream it out by name - so it moved to NVS
// (BlePasskeyStore.h). The retired bytes are kept in the struct only to keep the `.SV` layout
// (Enabled at 6, Display name at 8) readable, and this function is what makes sure they hold no
// secret: a key left there by an older build is carried into NVS (so an upgrade does not silently
// drop the owner's code) and the mirror is rewritten without it.
void BlePasskeyInit()
{
    BleSettingsPersistent *Per = &StaticPer.BleSet;
    BleSettingsVolatile *Vol = &StaticVol.BleSet;

    const bool RetiredHoldsSecret = BlePasskeyDigits(Per->ScrubbedPasskey) &&
                                    BlePasskeyEnabled(Per->ScrubbedPasskey);

    BlePasskeyStoreLoad(Vol->Passkey); // "000000" when nothing is stored

    if (RetiredHoldsSecret && !BlePasskeyEnabled(Vol->Passkey))
    {
        if (BlePasskeyStoreSave(Per->ScrubbedPasskey))
        {
            memcpy(Vol->Passkey, Per->ScrubbedPasskey, BLE_PASSKEY_LEN);
            DeviceLog("BLESEC", "passkey migrated from .SV to NVS");
        }
    }

    const bool LegalRemoval = memcmp(Per->ScrubbedPasskey, "000000", BLE_PASSKEY_LEN) != 0;
    // Not necessarily a secret (a device whose space was never normalised can hold six NULs), but
    // anything but the sentinel means the mirror predates this build - rewrite it either way.
    if (LegalRemoval)
    {
        memcpy(Per->ScrubbedPasskey, "000000", BLE_PASSKEY_LEN);
        // StaticSaveAll is the service's own path and is private; this is the same one-write
        // mirror it performs (RegisterPersist.h), plus the live net-ID it snapshots on the way.
        // Writing the file is the point: scrubbing only RAM would leave the old key in `.SV`
        // until the app happened to save.
#ifdef TYPE_CORE
        StaticPer.System.NetId = DeviceStatus.NetId;
#endif
        if (!WriteBackupFile(StaticValuesName(), (const uint8_t *)&StaticPer, sizeof(StaticPer)))
            DeviceLog("STORAGE", "could not rewrite .SV after scrubbing the retired passkey");
    }

    DeviceLog("BLESEC", "passkey source: %s", BlePasskeyEnabled(Vol->Passkey) ? "nvs (set)" : "none (just works)");
}

// Bond list refresh period. The array is re-formatted and compared once a second as a safety net
// (see BleSettingsSync); three 22-byte entries a second is nothing next to what used to be avoided by
// caching at all (a formatting allocation every ~2 ms pass).
#define BLE_BOND_ARRAY_CHECK_MS 1000

// Copies a fixed-width text field only when it actually differs, so a read-out that changed once is
// not rewritten every pass.
static void BleTextFieldSet(char *Field, size_t Len, const char *Text)
{
    if (strncmp(Field, Text, Len) == 0)
        return;
    size_t N = strlen(Text);
    if (N > Len)
        N = Len;
    memset(Field, 0x00, Len);
    memcpy(Field, Text, N);
}

// Rebuilds the block's `Bonds` array from the bond store: per slot, the peer's identity address
// (published order) followed by its stored name. An empty slot is six zero bytes and a blank name.
//
// The array is a VIEW, never a store: the address comes from NimBLE and the name from
// BleBondLabelStore.h, both keyed by the address, so eviction, a delete or a rename can never leave a
// name attached to the wrong phone. Labels whose bond is gone are dropped on the way (the label
// table is as wide as the store, so a stale one would otherwise be handed to the next bond).
static void BleBondArrayFormat()
{
    BleSettingsVolatile *Vol = &StaticVol.BleSet;
    uint8_t Live[BLE_SETTINGS_BOND_SLOTS * BLE_BOND_ADDR_LEN];
    int LiveCount = 0;

    for (int i = 0; i < BLE_SETTINGS_BOND_SLOTS; i++)
    {
        uint8_t *Entry = Vol->Bonds + i * BLE_BOND_ENTRY_LEN;
        uint8_t Addr[BLE_BOND_ADDR_LEN];
        if (!AppBLEBondAddress(i, Addr))
        {
            memset(Entry, 0x00, BLE_BOND_ENTRY_LEN); // no bond: the all-zero address IS the empty slot
            continue;
        }
        memcpy(Live + LiveCount * BLE_BOND_ADDR_LEN, Addr, BLE_BOND_ADDR_LEN);
        LiveCount++;

        memcpy(Entry, Addr, BLE_BOND_ADDR_LEN);
        char Name[BLE_BOND_NAME_LEN + 1];
        BleBondLabelGet(Addr, Name, sizeof(Name));
        // The name half is fixed-width and NUL-padded (no terminator guaranteed): a 16-character
        // label fills it exactly, which is why the field is not read as a C string.
        BleTextFieldSet((char *)Entry + BLE_BOND_ADDR_LEN, BLE_BOND_NAME_LEN, Name);
    }

    BleBondLabelPrune(Live, LiveCount);
}

// Called from the main loop. Everything the block owns is applied from here, so a register write
// takes effect within one loop iteration and the block stays the single source of truth:
//   - Enabled / Display name -> the radio (AppBLESettingsApply; the pairing policy is read at
//     every use by AppBLE.h, so it needs no push);
//   - Mode and the Bonds array <- the radio; Window <- the pairing deadline.
void BleSettingsSync()
{
    BleSettingsVolatile *Vol = &StaticVol.BleSet;

    AppBLESettingsApply();

    // Mode is derived from the passkey every pass, never stored: the two cannot disagree.
    Vol->Mode = BlePasskeyEnabled(Vol->Passkey) ? BLE_SET_MODE_PASSKEY : BLE_SET_MODE_JUST_WORKS;

    // The Bonds array, refreshed on the store's CONTENT rather than its size. A full store is the
    // case that makes the difference: a fourth phone makes nimble-cpp's store-status handler evict
    // the oldest bond and retry inside one callback (AppBLE.h, "Bond store read-out"), so
    // getNumBonds() reads 3 before and 3 after while the entries underneath are different. Keying
    // this on the count alone kept the evicted address in the block and never showed the phone that
    // had just paired. A `Bonds` write re-formats the array itself (OnBleBondsWrite), so a name or a
    // delete is visible in the same pass and not up to a second later.
    static BleBondRefresh BondRefresh;
    const int Bonds = AppBLEBondCount();
    const uint32_t NowMs = TimeFromBoot();
    if (BleBondRefreshDue(BondRefresh, AppBLEBondGeneration(), Bonds, NowMs))
    {
        BondRefresh.Generation = AppBLEBondGeneration();
        BondRefresh.Count = Bonds;
        BondRefresh.DueMs = NowMs + BLE_BOND_ARRAY_CHECK_MS;
        BleBondArrayFormat();
    }

    // The connectable window's countdown: the field reads what is left, a write sets the deadline.
    Vol->WindowMs = AppBLEWindowLeftMs();
}

// Field 0: the radio's enable state. USB-only like the rest of the block: switching the radio off
// is exactly the change a bonded phone must not be able to make, because it takes the device off
// the air it is being reached through. USB writes keep working with the radio down (the register
// path and the NVS store never depend on NimBLE), so an accidental `Enabled = false` is undone by
// one USB write - no reflash, no erase.
bool OnBleEnabledWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Index;
    if (Length != 1)
        return false;
    if (!BleBlockWritable())
    {
        DeviceLog("BLESEC", "Enabled refused: the block is written over USB only");
        return false;
    }

    BleSettingsPersistent *Per = (BleSettingsPersistent *)Block.PersistentData;
    Per->Enabled = *(const uint8_t *)Data ? 1 : 0;
    BleSettingsSync(); // apply it now, not on the next pass
    DeviceLog("BLESEC", "radio %s by the block", Per->Enabled ? "enabled" : "disabled");
    return true;
}

// Field 1: the passkey text. 6 ASCII digits, "000000" = Just Works. The write is stored in NVS
// (never in `.SV`) and applied at once: the next advertising cycle already carries the new policy,
// no reboot needed. The *_AUTHEN bits baked into the GATT table at init only catch up after a
// reboot, which the connect gate covers in the meantime (AppBLE.h re-checks the policy on every
// link, so a Just Works session is dropped the moment a passkey is set).
bool OnBlePasskeyWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Index;
    (void)Block;
    const char *Text = (const char *)Data;
    if (Length != BLE_PASSKEY_LEN || !BlePasskeyDigits(Text))
    {
        DeviceLog("BLESEC", "passkey refused: not %u ASCII digits", (unsigned)BLE_PASSKEY_LEN);
        return false;
    }
    if (!BleBlockWritable())
    {
        DeviceLog("BLESEC", "passkey refused: the block is written over USB only");
        return false;
    }
    if (!BlePasskeyStoreSave(Text))
        return false; // NVS refused: the policy is not changed either

    memcpy(StaticVol.BleSet.Passkey, Text, BLE_PASSKEY_LEN);

    BleSettingsSync();
    AppBLEApplySecurityPolicy();
    DeviceLog("BLESEC", "passkey written: mode=%s",
              BlePasskeyEnabled(StaticVol.BleSet.Passkey) ? "passkey (MITM)" : "just works");
    return true;
}

// Field 3: the advertised name. USB-only as well - a device whose name a phone can change is a
// device that can be made to impersonate another. The value is a plain copy into the persistent
// space (the generic path does not copy when a trigger runs); BleSettingsSync -> AppBLESettingsApply
// re-advertises in this same pass, so the air carries the new name before the reply is sent - the
// field IS the name, and "applied at the next advertising start" could mean never on a device that
// advertises whenever it is idle. An empty value means "use System.Name".
bool OnBleDisplayNameWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Index;
    if (Length != BLE_DISPLAY_NAME_LEN)
        return false;
    if (!BleBlockWritable())
    {
        DeviceLog("BLESEC", "Display name refused: the block is written over USB only");
        return false;
    }

    BleSettingsPersistent *Per = (BleSettingsPersistent *)Block.PersistentData;
    memcpy(Per->DisplayName, Data, BLE_DISPLAY_NAME_LEN);
    BleSettingsSync(); // the advertised name follows within this loop pass
    // The field is a fixed 16-byte text with no terminator: log a C string of the resolved name.
    char Name[SYSTEM_NAME_LEN + 1];
    BleEffectiveName(Name);
    DeviceLog("BLESEC", "display name written: advertised as '%s'", Name);
    return true;
}

// Field 4: the bond list, as BLE_SETTINGS_BOND_SLOTS entries of "6-byte address + peer name". The
// value is the WHOLE array (the register path requires the declared size for a non-Text field), so
// one write always describes every slot and the reply - the echoed frame, or FLAG_FAIL - describes
// the result of all of them.
//
// What a write may do, and what it cannot (Blocks/BleSettings.h states the same boundary):
//   - a NON-ZERO address may only repeat the address the store already holds in that slot; its NAME
//     is then writable (kept in NVS, BleBondLabelStore.h);
//   - an ALL-ZERO address deletes the bond that slot held;
//   - a non-zero address that the store does not hold in that slot is REFUSED for the whole frame,
//     before anything is applied - a register write cannot create a bond (the LTK/IRK live in the
//     store, never in the register file), and a fabricated entry would claim a peer that can never
//     authenticate. The refusal is FLAG_FAIL with no payload.
bool OnBleBondsWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Block;
    (void)Index;
    if (Length != BLE_BOND_ARRAY_LEN)
        return false;
    if (!BleBlockWritable())
    {
        DeviceLog("BLESEC", "Bonds refused: the block is written over USB only");
        return false;
    }

    const uint8_t *In = (const uint8_t *)Data;
    uint8_t Delete[BLE_SETTINGS_BOND_SLOTS][BLE_BOND_ADDR_LEN];
    int DeleteCount = 0;
    int Kept = 0;

    // 1. Decide the whole frame first: a refused write must not leave half of itself behind, so
    //    nothing is applied (names included) until every entry has been accounted for.
    for (int i = 0; i < BLE_SETTINGS_BOND_SLOTS; i++)
    {
        const uint8_t *Entry = In + i * BLE_BOND_ENTRY_LEN;
        uint8_t Cur[BLE_BOND_ADDR_LEN];
        const bool Has = AppBLEBondAddress(i, Cur);
        if (BleBondAddrEmpty(Entry))
        {
            if (Has)
            {
                memcpy(Delete[DeleteCount], Cur, BLE_BOND_ADDR_LEN);
                DeleteCount++;
            }
            continue;
        }
        Kept++;
        if (!Has || memcmp(Cur, Entry, BLE_BOND_ADDR_LEN) != 0)
        {
            DeviceLog("BLESEC", "Bonds refused: entry %d names an address the store does not hold", i);
            return false;
        }
    }

    // 2. The names of the entries that are staying.
    for (int i = 0; i < BLE_SETTINGS_BOND_SLOTS; i++)
    {
        const uint8_t *Entry = In + i * BLE_BOND_ENTRY_LEN;
        if (BleBondAddrEmpty(Entry))
            continue;
        if (!BleBondLabelSet(Entry, (const char *)Entry + BLE_BOND_ADDR_LEN, BLE_BOND_NAME_LEN))
        {
            DeviceLog("BLESEC", "Bonds refused: the name of entry %d could not be stored", i);
            return false;
        }
    }

    // 3. The deletions. An all-zero array is the forget-all path, which is where the bond epoch is
    //    stamped and the automatic window is owed - one implementation of "every bond is gone".
    if (Kept == 0)
    {
        if (AppBLEBondCount() > 0 && !AppBLEForgetAllBonds())
        {
            DeviceLog("BLESEC", "Bonds: the store did not come up empty");
            return false;
        }
    }
    else
    {
        for (int i = 0; i < DeleteCount; i++)
            if (!AppBLEForgetBond(Delete[i]))
            {
                DeviceLog("BLESEC", "Bonds: a bond could not be forgotten");
                return false;
            }
    }

    BleBondArrayFormat(); // the array is a view of the store, so it is rebuilt from it now
    BleSettingsSync();
    DeviceLog("BLESEC", "bonds written: %d kept, %d forgotten", Kept, DeleteCount);
    return true;
}

// Field 5: the connectable window. Reads the milliseconds LEFT; a write sets the deadline, and 0
// closes it. USB-only like every other write to the block - a phone must not be able to open its own
// pairing window. The `Action` enum that used to carry this command (and the forget-all) is gone:
// opening a window is a plain write of the period, and forgetting everything is zeroing `Bonds`.
bool OnBleWindowWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Block;
    (void)Index;
    if (Length != sizeof(uint32_t))
        return false;
    if (!BleBlockWritable())
    {
        DeviceLog("BLESEC", "Window refused: the block is written over USB only");
        return false;
    }

    uint32_t Ms = 0;
    memcpy(&Ms, Data, sizeof(Ms)); // the frame's value, little-endian like every Uint32
    if (!AppBLESetWindow(Ms))
        return false;

    BleSettingsSync(); // the read-back is the countdown, so it is refreshed now
    DeviceLog("BLESEC", "window write: %u ms", (unsigned)Ms);
    return true;
}
