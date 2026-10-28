#pragma once

// The device half of the `WiFi Settings` block (Blocks/WifiSettings.h): the write triggers, the boot
// load of the password store, and the per-loop hook that keeps the block's read-only fields honest.
// Included from Main.h (behind USE_WIFI_SETTINGS) after AppInterface.h, because the write rule reads
// the link a frame arrived on.
//
// WHO MAY CHANGE THE BLOCK: every write is refused unless the frame arrived over the USB link, the
// same rule as the BLE Settings block. AppInterface publishes the link a frame arrived on while it is
// dispatched (Core/Functions/AppInterface.h, AppFrameCameOverUsb), so the decision is based on the
// real interface and not on "which links happen to be up". A phone cannot set the network, point the
// device at a network of its choosing, or change the credential; with the password also unreadable
// (write-only, served from NVS), no credential is reachable over BLE in either direction.
//
// The radio does not exist: nothing here associates, scans or leases an address. The block persists
// its settings, keeps the password in NVS and reports the configuration's own state.

#include <cstring>
#include "Blocks/WifiSettings.h"
#include "WifiPasswordStore.h"

// Both are defined below and called from each other's neighbourhood: Init primes the store and then
// asks for one sync, so the syncing entry point is declared before its definition.
void WifiSettingsInit();
void WifiSettingsSync();

// Cached presence of the stored password: the one thing a reader may learn about it. Loaded once at
// boot and kept current by the triggers, so the per-loop sync never touches NVS.
static bool WifiPasswordStored = false;

// The block's one write rule.
static bool WifiBlockWritable()
{
    return AppFrameCameOverUsb();
}

// True when a fixed-width character field holds at least one character (it stops at the first NUL).
// The rule itself lives with the block (Blocks/WifiSettings.h WifiTextPresent) so the host test can
// drive the same decision the device uses.

// Boot: prime the password presence from NVS. Called after LoadAllBackups so the persistent fields
// (Enabled, SSID) already hold what `.SV` restored.
void WifiSettingsInit()
{
    WifiPasswordStored = WifiPasswordStorePresent();
    WifiSettingsSync();
    DeviceLog("WIFI", "settings: enabled=%u ssid=%s password=%s", (unsigned)StaticPer.WifiSet.Enabled,
              WifiTextPresent(StaticPer.WifiSet.Ssid, WIFI_SSID_LEN) ? "set" : "none",
              WifiPasswordStored ? "stored" : "none");
}

// Called from the main loop: the reported state follows the configuration, and the address stays
// empty because nothing leases one. Cheap on purpose (byte compares only) - it runs every pass so a
// register write is reflected within one loop iteration.
void WifiSettingsSync()
{
    WifiSettingsVolatile *Vol = &StaticVol.WifiSet;
    const WifiSettingsPersistent *Per = &StaticPer.WifiSet;

    const bool HasSsid = WifiTextPresent(Per->Ssid, WIFI_SSID_LEN);
    Vol->State = WifiStateFrom(Per->Enabled != 0, HasSsid, WifiPasswordStored);

    // No radio, so no address: all-zero is the documented "no address". Kept explicit (rather than
    // left to the initialiser) so a future radio card has exactly one place to write it and nothing
    // can leave a stale address behind in the meantime.
    memset(Vol->Ip, 0x00, WIFI_IP_LEN);

    // The password buffer is never filled - a read of the field must answer empty. Asserted here so
    // no future change can quietly start copying the text into the register file.
    memset(Vol->Password, 0x00, WIFI_PASSWORD_LEN);
}

// Field 0: Enabled. Persistent, and the write is refused unless it came over USB.
bool OnWifiEnabledWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Index;
    if (Length != 1)
        return false;
    if (!WifiBlockWritable())
    {
        DeviceLog("WIFI", "Enabled refused: the block is written over USB only");
        return false;
    }

    WifiSettingsPersistent *Per = (WifiSettingsPersistent *)Block.PersistentData;
    Per->Enabled = *(const uint8_t *)Data ? 1 : 0;
    WifiSettingsSync(); // the reported state follows now, not on the next pass
    // Switching the radio off does NOT forget the password: a toggle must not silently lose the
    // stored network. The forget paths are an empty password and clearing the SSID.
    DeviceLog("WIFI", "settings %s by the block", Per->Enabled ? "enabled" : "disabled");
    return true;
}

// Field 1: SSID. Persistent, USB-only. Clearing it also erases the stored password: a credential
// with no network to apply to is a secret kept for nothing.
bool OnWifiSsidWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Index;
    if (Length != WIFI_SSID_LEN)
        return false;
    if (!WifiBlockWritable())
    {
        DeviceLog("WIFI", "SSID refused: the block is written over USB only");
        return false;
    }

    WifiSettingsPersistent *Per = (WifiSettingsPersistent *)Block.PersistentData;
    memcpy(Per->Ssid, Data, WIFI_SSID_LEN);

    if (!WifiTextPresent(Per->Ssid, WIFI_SSID_LEN) && WifiPasswordStored)
    {
        if (WifiPasswordStoreErase())
        {
            WifiPasswordStored = false;
            DeviceLog("WIFI", "SSID cleared: stored password erased");
        }
    }

    WifiSettingsSync();
    DeviceLog("WIFI", "SSID %s by the block",
              WifiTextPresent(Per->Ssid, WIFI_SSID_LEN) ? "written" : "cleared");
    return true;
}

// Field 2: Password. Write-only and never persisted in the register file: the text goes to NVS and
// the volatile buffer stays empty. An empty value is the forget path.
//
// The value is not validated here - a WPA2 passphrase is 8..63 characters, but this build has no
// radio to reject a wrong one with, and the field is documented as "the text to try". The radio work
// validates it when it can report `5 Wrong password`.
bool OnWifiPasswordWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length)
{
    (void)Block;
    (void)Index;
    if (Length != WIFI_PASSWORD_LEN)
        return false;
    if (!WifiBlockWritable())
    {
        DeviceLog("WIFI", "password refused: the block is written over USB only");
        return false;
    }

    const char *Text = (const char *)Data;
    uint16_t Used = 0;
    while (Used < WIFI_PASSWORD_LEN && Text[Used] != '\0')
        Used++;

    if (Used == 0)
    {
        if (!WifiPasswordStoreErase())
            return false;
        WifiPasswordStored = false;
        WifiSettingsSync();
        DeviceLog("WIFI", "password erased by the block");
        return true;
    }

    if (!WifiPasswordStoreSave(Text, Used))
        return false; // NVS refused: the stored credential is not changed either

    WifiPasswordStored = true;
    // Never the text, never even its length beyond the bound: the log says only that it was written.
    DeviceLog("WIFI", "password stored (write-only; it does not read back)");
    WifiSettingsSync();
    return true;
}
