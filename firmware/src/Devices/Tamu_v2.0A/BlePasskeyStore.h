#pragma once

// Where the BLE passkey is kept: NVS, not the register file.
//
// The `BLE Settings` block's Passkey field is a secret, and the register machinery's persistence
// is `.SV` - a file in the `storage` (SPIFFS) partition that the Storage service will stream out by
// name to anyone who asks. A persistent register field is therefore protected from nothing but a
// flash dump (which no software can stop) while being readable over the protocol (which it can).
// So the value lives here instead: namespace `ble`, key `passkey`, a 6-byte blob of ASCII digits.
//
// `000000` is the "no passkey" sentinel (Just Works), and it ERASES the entry rather than storing
// six zeroes, so an absent key and a disabled key are the same thing on the device.
//
// This is deliberately the same treatment as the WiFi password (Devices/Tamu_v2.0A/WifiPasswordStore.h,
// Blocks/WifiSettings.h) - both live in NVS and never in `.SV` - but the opposite reader policy: a
// pairing code guards a local radio link and the owner wants the app to be able to show the user
// their own code, while a WiFi password is a network credential and is write-only (its register
// field always reads back empty). The asymmetry is a decision, not an oversight.

#include <cstring>
#include "Core/Functions/Log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "Blocks/BleSecurityPolicy.h"

#define BLE_PASSKEY_NVS_NS  "ble"
#define BLE_PASSKEY_NVS_KEY "passkey"

// Initialises the default NVS partition once. Idempotent, and it does not depend on NimBLE:
// a device booted with the radio disabled never initialises NimBLE, and its passkey still has to
// be readable and writable over USB.
static bool BlePasskeyStoreReady()
{
    static int State = -1; // -1 = untried, 0 = unusable, 1 = ready
    if (State >= 0)
        return State == 1;

    esp_err_t Rc = nvs_flash_init();
    if (Rc == ESP_ERR_NVS_NO_FREE_PAGES || Rc == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // A full or version-mismatched partition: the only way forward is to erase it. That also
        // drops the NimBLE bonds (they live in the same partition), which the log says plainly.
        DeviceLog("BLESEC", "nvs init rc=%d: erasing the partition (bonds included)", (int)Rc);
        nvs_flash_erase();
        Rc = nvs_flash_init();
    }
    State = (Rc == ESP_OK) ? 1 : 0;
    if (State == 0)
        DeviceLog("BLESEC", "nvs unavailable (rc=%d): the passkey cannot be stored", (int)Rc);
    return State == 1;
}

// Reads the stored passkey text into `Out` (exactly BLE_PASSKEY_LEN bytes, NOT NUL-terminated).
// A missing entry, a wrong-sized one or an unusable partition all read as "000000" (Just Works):
// the device must still come up and pair when the store is gone, and it must never claim a policy
// it cannot back with a key.
static void BlePasskeyStoreLoad(char *Out)
{
    memcpy(Out, "000000", BLE_PASSKEY_LEN);
    if (!BlePasskeyStoreReady())
        return;

    nvs_handle_t H;
    if (nvs_open(BLE_PASSKEY_NVS_NS, NVS_READONLY, &H) != ESP_OK)
        return; // no namespace yet: nothing has ever been set

    char Buf[BLE_PASSKEY_LEN];
    size_t Len = sizeof(Buf);
    const esp_err_t Rc = nvs_get_blob(H, BLE_PASSKEY_NVS_KEY, Buf, &Len);
    nvs_close(H);

    // Only a whole, well-formed value is accepted; anything else is treated as "not set" rather
    // than as a half-policy. The digits check is the same one the policy uses.
    if (Rc == ESP_OK && Len == BLE_PASSKEY_LEN && BlePasskeyDigits(Buf))
        memcpy(Out, Buf, BLE_PASSKEY_LEN);
    else if (Rc != ESP_ERR_NVS_NOT_FOUND)
        DeviceLog("BLESEC", "stored passkey ignored (rc=%d len=%u)", (int)Rc, (unsigned)Len);
}

// Stores `Text` (BLE_PASSKEY_LEN ASCII digits). "000000" erases the entry. Returns false when the
// partition refused the write, so the caller can refuse the register write instead of applying a
// policy that will be gone after the next boot.
static bool BlePasskeyStoreSave(const char *Text)
{
    if (!BlePasskeyDigits(Text))
        return false;
    if (!BlePasskeyStoreReady())
        return false;

    nvs_handle_t H;
    esp_err_t Rc = nvs_open(BLE_PASSKEY_NVS_NS, NVS_READWRITE, &H);
    if (Rc != ESP_OK)
    {
        DeviceLog("BLESEC", "nvs open failed (rc=%d)", (int)Rc);
        return false;
    }

    if (BlePasskeyEnabled(Text))
        Rc = nvs_set_blob(H, BLE_PASSKEY_NVS_KEY, Text, BLE_PASSKEY_LEN);
    else
        Rc = nvs_erase_key(H, BLE_PASSKEY_NVS_KEY); // `000000`: disabled == absent

    // An erase of an entry that was never there is the state we wanted anyway.
    if (Rc == ESP_ERR_NVS_NOT_FOUND)
        Rc = ESP_OK;
    if (Rc == ESP_OK)
        Rc = nvs_commit(H);
    nvs_close(H);

    if (Rc != ESP_OK)
        DeviceLog("BLESEC", "passkey store write failed (rc=%d)", (int)Rc);
    return Rc == ESP_OK;
}

// ===== Which policy the stored bonds were made under =====
//
// A bond is only worth anything under the passkey it was created with. A phone reconnecting with
// its stored LTK comes up encrypted and bonded but NOT authenticated, so after a passkey change the
// connect gate refuses it - forever, because a phone does not re-pair on its own while it believes
// it is bonded (nimble's repeat-pairing path ignores the request until the application deletes the
// bond) and the only other way out is the USB-only Action=1. The same hole is a security one: a
// bond made under the OLD code would survive a rotation, so rotating the code would revoke nothing.
//
// So the passkey the store was built under is remembered next to it, and any change forgets the
// bonds it invalidates (AppBLEEnforceBondPolicy in Devices/Tamu_v2.0A/AppBLE.h, called when the
// policy is applied and again at radio start).
//
// It has to be persistent, not RAM: `Enabled = false` is a legal USB write, so the passkey can be
// changed while the radio - and with it the bond store's API - is down. The next radio start
// applies the change, which for a disabled radio is the same boot.
//
// The stamp is the 6-byte passkey TEXT, not a hash: it is not a secret (the block serves the
// passkey to its reader, see Blocks/BleSettings.h) and comparing texts cannot collide. Unlike the
// passkey, the "000000" sentinel IS stored here - an absent stamp means "unknown", which is a
// third state: a fresh partition, or a device upgraded from a build that predates this rule. The
// reader resolves it by policy (AppBLEEnforceBondPolicy): the only build that could have written
// such a store had no passkey at all, so its bonds are kept under Just Works and revoked once if
// the device now runs with a passkey, which is the safe direction.
#define BLE_BOND_EPOCH_NVS_KEY "bondpass"

// Reads the stamp. False when none is stored (see above); the caller treats that as "the bonds do
// not match the current policy".
static bool BleBondEpochLoad(char *Out)
{
    if (!BlePasskeyStoreReady())
        return false;

    nvs_handle_t H;
    if (nvs_open(BLE_PASSKEY_NVS_NS, NVS_READONLY, &H) != ESP_OK)
        return false;

    char Buf[BLE_PASSKEY_LEN];
    size_t Len = sizeof(Buf);
    const esp_err_t Rc = nvs_get_blob(H, BLE_BOND_EPOCH_NVS_KEY, Buf, &Len);
    nvs_close(H);

    // A key that is not a whole passkey text is not a stamp either: treat it as unknown.
    if (Rc != ESP_OK || Len != BLE_PASSKEY_LEN || !BlePasskeyDigits(Buf))
        return false;
    memcpy(Out, Buf, BLE_PASSKEY_LEN);
    return true;
}

// Records `Text` as the policy the bond store is under. False when the partition refused it; the
// caller keeps the value in RAM, so the comparison stays right until the next boot re-reads it.
static bool BleBondEpochSave(const char *Text)
{
    if (!BlePasskeyDigits(Text))
        return false;
    if (!BlePasskeyStoreReady())
        return false;

    nvs_handle_t H;
    if (nvs_open(BLE_PASSKEY_NVS_NS, NVS_READWRITE, &H) != ESP_OK)
        return false;

    esp_err_t Rc = nvs_set_blob(H, BLE_BOND_EPOCH_NVS_KEY, Text, BLE_PASSKEY_LEN);
    if (Rc == ESP_OK)
        Rc = nvs_commit(H);
    nvs_close(H);

    if (Rc != ESP_OK)
        DeviceLog("BLESEC", "bond policy stamp write failed (rc=%d)", (int)Rc);
    return Rc == ESP_OK;
}
