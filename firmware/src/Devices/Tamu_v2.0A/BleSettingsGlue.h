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

static_assert(BLE_SETTINGS_BOND_SLOTS == CONFIG_BT_NIMBLE_MAX_BONDS,
              "the Bond field must expose exactly the bond store's capacity");

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

// Bond text refresh period. The entries are re-formatted and compared once a second as a safety
// net (see BleSettingsSync); three 18-byte addresses a second is nothing next to what used to be
// avoided by caching at all (a formatting allocation every ~2 ms pass).
#define BLE_BOND_TEXT_CHECK_MS 1000

// One address of the last judged link, with the TYPE the controller reported for it:
// "AA:BB:CC:DD:EE:FF/N". NimBLEAddress keeps the six bytes least-significant first, which is why they
// are printed in reverse (same order the bond texts come out in, so the two read-outs line up by eye).
// 19 characters, so it fits BLE_BOND_TEXT_LEN (20 with the terminator) and the wire layout does not
// move. N=0/1 is the address as it came off the air (public / random, an unresolved RPA included);
// N=2/3 is an IDENTITY the controller resolved a rotating private address back to, which is the only
// resolution witness NimBLE exposes on a controller-based-privacy build (Issues.md E66:
// `getAddress()` and `getIdAddress()` are the same address there, so an address PAIR can never show
// it). Formatted into a wider buffer and clamped: GCC cannot bound a promoted uint8_t through a %X
// directive, so a direct snprintf into the 20-byte field trips -Werror=format-truncation.
static void BleAddrTypeText(const uint8_t *Val, uint8_t Type, char *Out, size_t Len)
{
    char Full[32];
    snprintf(Full, sizeof(Full), "%02X:%02X:%02X:%02X:%02X:%02X/%u",
             Val[5], Val[4], Val[3], Val[2], Val[1], Val[0], (unsigned)(Type % 10));
    size_t N = strlen(Full);
    if (N >= Len)
        N = Len - 1;
    memcpy(Out, Full, N);
    Out[N] = '\0';
}

// Copies a fixed-width text field only when it actually differs, so a read-out that changed once is
// not rewritten every pass (the rule the Bond texts above follow).
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

// Called from the main loop. Everything the block owns is applied from here, so a register write
// takes effect within one loop iteration and the block stays the single source of truth:
//   - Enabled / Display name -> the radio (AppBLESettingsApply; the pairing policy is read at
//     every use by AppBLE.h, so it needs no push);
//   - the read-only fields <- the radio and the displayed name <- the block.
void BleSettingsSync()
{
    BleSettingsVolatile *Vol = &StaticVol.BleSet;

    AppBLESettingsApply();

    // Mode is derived from the passkey every pass, never stored: the two cannot disagree.
    Vol->Mode = BlePasskeyEnabled(Vol->Passkey) ? BLE_SET_MODE_PASSKEY : BLE_SET_MODE_JUST_WORKS;

    const int Bonds = AppBLEBondCount();
    Vol->BondCount = (uint8_t)(Bonds < 0 ? 0 : (Bonds > 255 ? 255 : Bonds));

    // The bond texts, refreshed on the store's CONTENT rather than its size. A full store is the
    // case that makes the difference: a fourth phone makes nimble-cpp's store-status handler evict
    // the oldest bond and retry inside one callback (AppBLE.h, "Bond store read-out"), so
    // getNumBonds() reads 3 before and 3 after while the entries underneath are different. Keying
    // this on the count alone kept the evicted address in the block and never showed the phone that
    // had just paired.
    //
    // The predicate (BleBondRefreshDue, Blocks/BleSettings.h) is: the bond store's generation
    // changed - bumped by every completed bond and by Action=1, so the common changes show up in
    // the same loop pass - or the count changed, or a ~1 Hz content check is due for a store that
    // changed without either event (a phone that removed its own bond, the stack unpairing on a
    // failed encryption). Formatting allocates, so this never runs every pass; each entry is copied
    // only when it actually differs. Reading the store from this task is safe: getBondedAddress()
    // walks it through ble_store_read(), which takes the NimBLE host lock, so the 1 Hz check can
    // never observe a half-written entry.
    static BleBondRefresh BondRefresh;
    const uint32_t NowMs = TimeFromBoot();
    if (BleBondRefreshDue(BondRefresh, AppBLEBondGeneration(), Bonds, NowMs))
    {
        BondRefresh.Generation = AppBLEBondGeneration();
        BondRefresh.Count = Bonds;
        BondRefresh.DueMs = NowMs + BLE_BOND_TEXT_CHECK_MS;
        for (int i = 0; i < BLE_SETTINGS_BOND_SLOTS; i++)
        {
            // AppBLEBondText() writes a NUL-terminated address (17 chars) or leaves the buffer
            // empty; BLE_BOND_TEXT_LEN holds either.
            char Text[BLE_BOND_TEXT_LEN];
            if (!AppBLEBondText(i, Text, BLE_BOND_TEXT_LEN))
                Text[0] = '\0';
            if (strncmp(Text, Vol->Bond[i], BLE_BOND_TEXT_LEN) == 0)
                continue;
            size_t Len = strlen(Text);
            if (Len >= BLE_BOND_TEXT_LEN)
                Len = BLE_BOND_TEXT_LEN - 1;
            memset(Vol->Bond[i], 0x00, BLE_BOND_TEXT_LEN);
            memcpy(Vol->Bond[i], Text, Len);
        }
    }

    Vol->WindowMs = AppBLEWindowLeftMs();

    // The resolved advertised name: the Display name, or System.Name when that is empty. Kept in
    // the block so the app can show what is actually on the air without re-implementing the rule.
    char Name[SYSTEM_NAME_LEN + 1];
    BleEffectiveName(Name);
    if (strncmp(Vol->AdvertisedName, Name, BLE_DISPLAY_NAME_LEN) != 0)
    {
        memset(Vol->AdvertisedName, 0x00, BLE_DISPLAY_NAME_LEN);
        size_t Len = strlen(Name);
        if (Len > BLE_DISPLAY_NAME_LEN)
            Len = BLE_DISPLAY_NAME_LEN;
        memcpy(Vol->AdvertisedName, Name, Len);
    }

    // The last judged link (Issues.md E66): the identity the admission used, the address the peer came
    // from, and how the link ended. Written by the radio callbacks and copied here, so the block is
    // only ever written from this task. LastPeer != LastAir is the controller having resolved a
    // rotating private address - the fact E66 turns on and the log cannot show (E67).
    BleLastLinkRecord LastLink;
    if (AppBLELastLink(&LastLink))
    {
        char PeerText[BLE_BOND_TEXT_LEN];
        char AirText[BLE_BOND_TEXT_LEN];
        BleAddrTypeText(LastLink.Id, LastLink.IdType, PeerText, sizeof(PeerText));
        BleAddrTypeText(LastLink.Air, LastLink.AirType, AirText, sizeof(AirText));
        BleTextFieldSet(Vol->LastPeer, BLE_BOND_TEXT_LEN, PeerText);
        BleTextFieldSet(Vol->LastAir, BLE_BOND_TEXT_LEN, AirText);
        Vol->LastLink = LastLink.Verdict;
    }

    // The command byte is a trigger, never a stored value; keep it clear so a read-back cannot
    // claim a command is still pending.
    Vol->Action = 0;
}

// Field 0: the passkey text. 6 ASCII digits, "000000" = Just Works. The write is stored in NVS
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

// Field 6: the radio's enable state. USB-only like the rest of the block: switching the radio off
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

// Field 7: the advertised name. USB-only as well - a device whose name a phone can change is a
// device that can be made to impersonate another. The value is a plain copy into the persistent
// space (the generic path does not copy when a trigger runs), applied on the next advertising
// start by BleSettingsSync -> AppBLESettingsApply. An empty value means "use System.Name".
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
    BleSettingsSync(); // the advertised name follows within this loop
    // The field is a fixed 16-byte text with no terminator: log a C string of the resolved name.
    char Name[SYSTEM_NAME_LEN + 1];
    BleEffectiveName(Name);
    DeviceLog("BLESEC", "display name written: advertised as '%s'", Name);
    return true;
}

// Field 5: the only command surface. 0 opens the pairing window, 1 forgets every bond; anything
// else is refused rather than ignored. USB-only like every other write to the block - the phone
// cannot open its own pairing window, and for the same reason the forget cannot be triggered from
// a phone that would then re-pair itself.
bool OnBleActionWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Block;
    (void)Index;
    if (Length != 1)
        return false;
    if (!BleBlockWritable())
    {
        DeviceLog("BLESEC", "Action refused: the block is written over USB only");
        return false;
    }

    const uint8_t Action = *(const uint8_t *)Data;
    switch (Action)
    {
    case BLE_SET_ACTION_OPEN_WINDOW:
        if (!AppBLEOpenWindow())
            return false;
        BleSettingsSync();
        return true;

    case BLE_SET_ACTION_FORGET_BONDS:
        // Destructive: every paired phone has to pair again. The empty bond store re-opens the
        // window automatically for its 5-minute recovery period (AppBLE.h), so the device is
        // immediately pairable again without a second command.
        if (!AppBLEForgetAllBonds())
            return false;
        BleSettingsSync();
        return true;

    default:
        DeviceLog("BLESEC", "unknown action %u refused", (unsigned)Action);
        return false;
    }
}
