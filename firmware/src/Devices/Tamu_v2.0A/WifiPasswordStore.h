#pragma once

// Where the WiFi password is kept: NVS, not the register file - and write-only, unlike the BLE
// passkey.
//
// The `WiFi Settings` block's Password field is a network credential, and the register machinery's
// persistence is `.SV`, a file in the `storage` (SPIFFS) partition that the Storage service will
// stream out by name to anyone who asks. A persistent register field would therefore be readable
// over the protocol by exactly the reader a credential has to be kept from. So the value lives
// here: namespace `wifi`, key `password`, a blob of the text's own bytes (1..WIFI_PASSWORD_LEN).
//
// The register field is still a Trigger (Blocks/WifiSettings.h): a write goes to the trigger, which
// calls this store and leaves the volatile buffer EMPTY, so a read of Password answers 64 zero
// bytes. There is no path by which the stored text comes back out of the register file, and the
// block's presence flag is a cached bool, not the value.
//
// This is the same policy as the BLE passkey (Devices/Tamu_v2.0A/BlePasskeyStore.h): a secret lives
// in NVS and is never mirrored into the storage partition. The difference is the reader: a passkey
// guards a local radio link and stays readable so the app can show the owner their own code, while
// a WiFi password is a network credential and is write-only.
//
// An empty password (and clearing the SSID, which leaves the credential nothing to apply to) ERASES
// the entry rather than storing an empty blob, so "no password" has exactly one representation on
// the device. A missing entry reads as absent - which is what `State = 4 No password` reports.

#include <cstdint>
#include <cstring>
#include "Core/Functions/Log.h"
#include "Blocks/WifiSettings.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_PASS_NVS_NS  "wifi"
#define WIFI_PASS_NVS_KEY "password"

// Initialises the default NVS partition once. Idempotent (and shared with the BLE passkey store:
// nvs_flash_init on an already-initialised partition answers ESP_OK), and it does not depend on
// the radio - a device with WiFi compiled in but switched off still has to store and forget a
// password over USB.
static bool WifiPasswordStoreReady()
{
    static int State = -1; // -1 = untried, 0 = unusable, 1 = ready
    if (State >= 0)
        return State == 1;

    esp_err_t Rc = nvs_flash_init();
    if (Rc == ESP_ERR_NVS_NO_FREE_PAGES || Rc == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // A full or version-mismatched partition: the only way forward is to erase it. That also
        // drops whatever else lives in it (the NimBLE bonds and the BLE passkey), which the log
        // says plainly.
        DeviceLog("WIFI", "nvs init rc=%d: erasing the partition (bonds and passkey included)", (int)Rc);
        nvs_flash_erase();
        Rc = nvs_flash_init();
    }
    State = (Rc == ESP_OK) ? 1 : 0;
    if (State == 0)
        DeviceLog("WIFI", "nvs unavailable (rc=%d): the password cannot be stored", (int)Rc);
    return State == 1;
}

// Whether a password is stored. This is the whole of what a reader may learn about it: the presence
// feeds `State = 4 No password`, never the text.
static bool WifiPasswordStorePresent()
{
    if (!WifiPasswordStoreReady())
        return false;

    nvs_handle_t H;
    if (nvs_open(WIFI_PASS_NVS_NS, NVS_READONLY, &H) != ESP_OK)
        return false; // no namespace yet: nothing has ever been set

    size_t Len = 0;
    const esp_err_t Rc = nvs_get_blob(H, WIFI_PASS_NVS_KEY, nullptr, &Len);
    nvs_close(H);
    if (Rc != ESP_OK && Rc != ESP_ERR_NVS_INVALID_LENGTH)
    {
        if (Rc != ESP_ERR_NVS_NOT_FOUND)
            DeviceLog("WIFI", "stored password unreadable (rc=%d)", (int)Rc);
        return false;
    }
    // A zero-length or oversized blob is not a password this build wrote; treat it as absent rather
    // than as a half-credential.
    if (Len == 0 || Len > WIFI_PASSWORD_LEN)
    {
        DeviceLog("WIFI", "stored password ignored (len=%u)", (unsigned)Len);
        return false;
    }
    return true;
}

// Erases the stored password. Returns false only when the partition refused the operation; an entry
// that was never there is the state we wanted anyway.
static bool WifiPasswordStoreErase()
{
    if (!WifiPasswordStoreReady())
        return false;

    nvs_handle_t H;
    esp_err_t Rc = nvs_open(WIFI_PASS_NVS_NS, NVS_READWRITE, &H);
    if (Rc != ESP_OK)
    {
        DeviceLog("WIFI", "nvs open failed (rc=%d)", (int)Rc);
        return false;
    }
    Rc = nvs_erase_key(H, WIFI_PASS_NVS_KEY);
    if (Rc == ESP_ERR_NVS_NOT_FOUND)
        Rc = ESP_OK;
    if (Rc == ESP_OK)
        Rc = nvs_commit(H);
    nvs_close(H);

    if (Rc != ESP_OK)
        DeviceLog("WIFI", "password erase failed (rc=%d)", (int)Rc);
    return Rc == ESP_OK;
}

// Stores `Len` bytes of `Text` (1..WIFI_PASSWORD_LEN; the caller passes the text up to its first
// NUL, not the NUL-padded field). Returns false when the partition refused the write, so the caller
// can refuse the register write instead of reporting a credential the next boot will not find.
static bool WifiPasswordStoreSave(const char *Text, uint16_t Len)
{
    if (Len == 0 || Len > WIFI_PASSWORD_LEN || Text == nullptr)
        return false;
    if (!WifiPasswordStoreReady())
        return false;

    nvs_handle_t H;
    esp_err_t Rc = nvs_open(WIFI_PASS_NVS_NS, NVS_READWRITE, &H);
    if (Rc != ESP_OK)
    {
        DeviceLog("WIFI", "nvs open failed (rc=%d)", (int)Rc);
        return false;
    }
    Rc = nvs_set_blob(H, WIFI_PASS_NVS_KEY, Text, Len);
    if (Rc == ESP_OK)
        Rc = nvs_commit(H);
    nvs_close(H);

    // The value never reaches a log line: the length is all that is reported.
    if (Rc != ESP_OK)
        DeviceLog("WIFI", "password store write failed (rc=%d)", (int)Rc);
    return Rc == ESP_OK;
}
