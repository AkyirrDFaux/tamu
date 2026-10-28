#pragma once

// The device half of the `BLE Settings` block (Blocks/BleSettings.h): the two write triggers and
// the per-loop hook that keeps the block's read-only fields level with the radio. Included from
// Main.h AFTER AppBLE.h, because it drives the link.
//
// Who may change the policy: the passkey and "forget all bonds" are accepted only while the USB
// link is the live app interface. A bonded phone that could rotate the key would defeat the
// reason the key exists, and the register service cannot see which link a write arrived on, so
// the rule is stated in terms of the interface that is up: a BLE-only session is refused. The
// cost of that proxy is the case "USB host attached but silent for USB_TX_SILENCE_MS" - a write
// from the phone then waits for the next USB transaction, which costs nothing in practice and
// errs on the safe side. Opening the window is accepted from any link: it grants nothing but the
// chance to pair, and pairing still meets the current policy.

#include "Blocks/BleSettings.h"

static_assert(BLE_SETTINGS_BOND_SLOTS == CONFIG_BT_NIMBLE_MAX_BONDS,
              "the Bond field must expose exactly the bond store's capacity");

// True while the USB link is the one carrying app traffic (see the note above).
static bool BleProvisioningLink()
{
    return AppUSBActive() && !AppUsbSilent();
}

// Called from the main loop. Everything the block owns is applied from here, so a register write
// takes effect within one loop iteration and the block stays the single source of truth:
//   - Enabled / Display name -> the radio (AppBLESettingsApply; the pairing policy is read at
//     every use by AppBLE.h, so it needs no push);
//   - the read-only fields <- the radio.
void BleSettingsSync()
{
    BleSettingsPersistent *Per = &StaticPer.BleSet;
    BleSettingsVolatile *Vol = &StaticVol.BleSet;

    // A persistent space written by an older build (or one whose new tail was never saved)
    // carries a non-digit passkey - and, in that same zero tail, Enabled = 0, which would read as
    // "radio off" for a device nobody ever configured. The passkey is the marker: a saved space
    // always holds six digits. Normalise the whole tail to the block's defaults (Just Works, radio
    // on) so a read-back shows "no passkey" instead of six NULs.
    if (!BlePasskeyDigits(Per->Passkey))
    {
        memcpy(Per->Passkey, "000000", BLE_PASSKEY_LEN);
        Per->Enabled = 1;
    }

    AppBLESettingsApply();

    Vol->Mode = BlePasskeyEnabled(Per->Passkey) ? BLE_SET_MODE_PASSKEY : BLE_SET_MODE_JUST_WORKS;

    const int Bonds = AppBLEBondCount();
    Vol->BondCount = (uint8_t)(Bonds < 0 ? 0 : (Bonds > 255 ? 255 : Bonds));

    // The bond texts are re-formatted only when the store's size changes: formatting an address
    // allocates, and this runs every loop.
    static int SyncedBonds = -1;
    if (Bonds != SyncedBonds)
    {
        SyncedBonds = Bonds;
        for (int i = 0; i < BLE_SETTINGS_BOND_SLOTS; i++)
        {
            if (!AppBLEBondText(i, Vol->Bond[i], BLE_BOND_TEXT_LEN))
                Vol->Bond[i][0] = '\0';
        }
    }

    Vol->WindowMs = AppBLEWindowLeftMs();

    // The command byte is a trigger, never a stored value; keep it clear so a read-back cannot
    // claim a command is still pending.
    Vol->Action = 0;
}

// Field 0: the passkey text - the single source of truth for the pairing policy. Writing it
// takes effect on the next pairing; the attribute permissions baked into the GATT table at init
// only catch up after a reboot, which the connect gate covers in the meantime (AppBLE.h).
bool OnBlePasskeyWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Index;
    const char *Text = (const char *)Data;
    if (Length != BLE_PASSKEY_LEN || !BlePasskeyDigits(Text))
    {
        DeviceLog("BLESEC", "passkey refused: not %u ASCII digits", (unsigned)BLE_PASSKEY_LEN);
        return false;
    }
    if (!BleProvisioningLink())
    {
        DeviceLog("BLESEC", "passkey refused: the USB link is not the live interface");
        return false;
    }

    // The trigger applies the write itself (the generic path does not copy when a trigger runs).
    BleSettingsPersistent *Per = (BleSettingsPersistent *)Block.PersistentData;
    memcpy(Per->Passkey, Text, BLE_PASSKEY_LEN);

    BleSettingsSync();
    AppBLEApplySecurityPolicy();
    DeviceLog("BLESEC", "passkey written: mode=%s",
              BlePasskeyEnabled(Per->Passkey) ? "passkey (MITM)" : "just works");
    return true;
}

// Field 6: the radio's enable state. USB-only for the same reason as the passkey: switching the
// radio off is exactly the kind of change a bonded phone must not be able to make, because it
// takes the device off the air it is being reached through.
bool OnBleEnabledWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Index;
    if (Length != 1)
        return false;
    if (!BleProvisioningLink())
    {
        DeviceLog("BLESEC", "Enabled refused: the USB link is not the live interface");
        return false;
    }

    BleSettingsPersistent *Per = (BleSettingsPersistent *)Block.PersistentData;
    Per->Enabled = *(const uint8_t *)Data ? 1 : 0;
    BleSettingsSync(); // apply it now, not on the next pass
    DeviceLog("BLESEC", "radio %s by the block", Per->Enabled ? "enabled" : "disabled");
    return true;
}

// Field 5: the only command surface. 0 opens the pairing window, 1 forgets every bond; anything
// else is refused rather than ignored.
bool OnBleActionWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Block;
    (void)Index;
    if (Length != 1)
        return false;

    const uint8_t Action = *(const uint8_t *)Data;
    switch (Action)
    {
    case BLE_SET_ACTION_OPEN_WINDOW:
        // Accepted from any link: the whole point is to open the window from the phone.
        if (!AppBLEOpenWindow())
            return false;
        BleSettingsSync();
        return true;

    case BLE_SET_ACTION_FORGET_BONDS:
        // Destructive: every paired phone has to pair again. USB only, like the passkey itself.
        if (!BleProvisioningLink())
        {
            DeviceLog("BLESEC", "forget bonds refused: the USB link is not the live interface");
            return false;
        }
        if (!AppBLEForgetAllBonds())
            return false;
        BleSettingsSync();
        return true;

    default:
        DeviceLog("BLESEC", "unknown action %u refused", (unsigned)Action);
        return false;
    }
}
