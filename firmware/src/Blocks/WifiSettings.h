#pragma once

// ===== WiFi Settings (compile-in option) =====
// The WiFi link's settings and state as register fields, so it is visible and settable through the
// register path the app already speaks instead of a side-channel command set - the same shape the
// BLE Settings block (Blocks/BleSettings.h) has for the radio link. Block type 0x0C, one instance,
// core only.
//
//   Enabled     (0, 0)  Bool, Trigger|Persistent   associate when set
//   SSID        (1, 0)  Text(32), Persistent       the network to join
//   Password    (2, 0)  Text(64), Trigger          WRITE-ONLY: always reads back empty; NVS, not .SV
//   State       (3, 0)  Enum, RO                   0 Disconnected .. 6 Error (see below)
//   IP address  (4, 0)  Undefined(16), RO          the address in network order, all-zero = none
//
// Field numbers run in the order of that table (the owner's table lists the keys, not the field
// indexes; this is the one place they are fixed).
//
// ===== The radio is NOT implemented =====
// This block is configuration and reporting only: there is no station, no scan, no association and
// no DHCP behind it. What it does today is persist Enabled and SSID, keep the password in NVS, and
// report the configuration's own state. `State` therefore reports only what a configuration check
// can prove - 0 Disconnected (enabled, SSID and password present, not associated), 3 No SSID and
// 4 No password - and NEVER 1 Connecting, 2 Connected or 5 Wrong password: those are results of a
// radio this build does not have, and claiming them would be a lie the app would draw. `IP address`
// stays all-zero. Nobody should read this block as "WiFi works now".
//
// ===== Compile-in option =====
// Deliberately behind `USE_WIFI_SETTINGS` (a build flag in platformio.ini, not a board ifdef), and
// OFF in every shipped environment: Tamu is a USB/BLE device and WiFi is not used by default. With
// the flag off, neither this header's block nor its fields exist - StaticBlockNum does not grow, so
// the block is ABSENT from enumeration rather than present-but-empty, and the app's registry and any
// old client see exactly what they saw before. Field 8 of the System block never sets its WiFi bit
// either way (AppActiveBits::WiFi is reserved until the UDP app transport of
// Docs/Services/App Interface.md is implemented).
//
// The block sits LAST in the board's static persistent space (after the BLE Settings tail), because
// `.SV` is the whole space mirrored 1:1: appending keeps every other field's offset, and a mirror
// written by a WiFi-enabled build decodes as a valid prefix on a build without the option. The
// consequence of toggling the option off (or a mirror from one build meeting the other) is that the
// WiFi fields fall back to their compiled-in defaults - nothing else in the space moves.
//
// ===== The password: write-only, in NVS, never in `.SV` =====
// The register machinery's persistence is `.SV`, a file in the `storage` (SPIFFS) partition, and the
// Storage service streams files out by name to anyone who asks - so a persistent field is a
// credential readable over the very protocol that carries the block. The password is therefore NOT a
// persistent register field:
//   - its register entry is a Trigger into the volatile space, so a write goes to the trigger and
//     never into the register file; the trigger stores it in NVS (namespace `wifi`, key `password`;
//     Devices/Tamu_v2.0A/WifiPasswordStore.h) and leaves the volatile buffer EMPTY;
//   - the volatile buffer is never filled, so a read of Password always answers 64 zero bytes -
//     there is no path by which the stored text can come back out of the register file;
//   - the presence of a password (needed for State 4 No password) is a cached bool, not the value.
// This is the same rule as the BLE passkey (t_a9cda0b9 / Blocks/BleSecurityPolicy.h): one policy for
// every secret on the device - a secret lives in NVS and is never mirrored into the storage
// partition. The difference is that a passkey guards a local radio link and stays readable (the app
// may show the owner their own code), while a WiFi password is a network credential and is
// write-only.
// An empty Password write, and SSID being cleared, are the forget paths: both erase the NVS entry
// (no network left for the credential to apply to). `Enabled = false` does NOT erase it - switching
// the radio off and back on must not silently lose the stored network.
//
// ===== Who may write =====
// EVERY write to this block is refused unless the frame arrived over the USB link (the same rule as
// the BLE Settings block). The source is not inferred from which interfaces happen to be up:
// Core/Functions/AppInterface.h publishes the link a frame arrived on while it is dispatched
// (AppFrameCameOverUsb), so a write from a phone - bonded, authenticated or not - gets a normal
// FLAG_FAIL reply. Combined with the empty read-back, no credential is reachable over the BLE link.
//
// ===== IP address =====
// 16 raw bytes in NETWORK order (the wire's own byte order never enters the picture; the firmware
// does not format the value at all - the app renders the standard grouped-hex form). All-zero means
// "no address". An IPv4-only network is reported IPv4-MAPPED (`::ffff:a.b.c.d`, i.e. bytes 10 and 11
// set to 0xFF) so one field carries both families and a client has exactly one decoding path. IPv6
// needs no build change: CONFIG_LWIP_IPV6 is on (CONFIG_LWIP_IPV6_NUM_ADDRESSES = 3 in
// sdkconfig.Tamu_v2_0A); this field is simply wide enough to hold either family.

#include <cstddef>
#include <cstdint>
#include "Core/Functions/MemoryTypes.h"

// WiFi SSID: the 802.11 limit, and the app's own maxChars (app/lib/core/block_registry.dart).
#define WIFI_SSID_LEN 32
// A WPA2 passphrase is at most 63 characters; the field is 4-aligned, so 64 bytes with the last
// byte as padding/NUL. The meaningful text ends at the first NUL.
#define WIFI_PASSWORD_LEN 64
// One address: IPv6's 16 bytes, which also carry an IPv4 address in its IPv4-mapped form.
#define WIFI_IP_LEN 16

// Offsets into the two value spaces (see StaticBlockDescriptor::Base).
#define WIFI_SET_FIELD_ENABLED  0
#define WIFI_SET_FIELD_SSID     1
#define WIFI_SET_FIELD_PASSWORD 2
#define WIFI_SET_FIELD_STATE    3
#define WIFI_SET_FIELD_IP       4

// State, as the docs number it (Docs/Services/App Interface.md "WiFi Settings").
#define WIFI_STATE_DISCONNECTED   0 // enabled, configuration complete, not associated
#define WIFI_STATE_CONNECTING     1 // never reported: no radio
#define WIFI_STATE_CONNECTED      2 // never reported: no radio
#define WIFI_STATE_NO_SSID        3 // enabled, no SSID to join
#define WIFI_STATE_NO_PASSWORD    4 // SSID present, no password stored
#define WIFI_STATE_WRONG_PASSWORD 5 // never reported: needs an association attempt
#define WIFI_STATE_ERROR          6 // never reported: needs a radio

struct WifiSettingsPersistent
{
    // Off by default: a device whose WiFi option was compiled in but never configured must not try
    // to associate with anything. (The option itself is off in every shipped build.)
    uint8_t Enabled = 0;                                    // field 0, offset 0
    // alignas(4): a value is aligned to its size capped at 4 (Docs/Services/Register.md "Memory with
    // 32-bit alignment"), so the 32-byte SSID sits at 4. NUL-padded, no terminator of its own.
    alignas(4) char Ssid[WIFI_SSID_LEN] = {};               // field 1, offset 4
};

struct WifiSettingsVolatile
{
    // Always empty: the write trigger stores the text in NVS and never copies it here, so a register
    // read of Password cannot return a credential (see the header note).
    alignas(4) char Password[WIFI_PASSWORD_LEN] = {};       // field 2, offset 0
    uint8_t State = WIFI_STATE_DISCONNECTED;                // field 3, offset 64
    // The address, in network order. Written by nothing yet: all-zero is "no address", which is what
    // a device with no radio truthfully reports.
    alignas(4) uint8_t Ip[WIFI_IP_LEN] = {};                // field 4, offset 68
};

// The entry table addresses the value spaces by byte offset, so the structs and the offsets above
// must agree. Pinned here rather than discovered as a read that returns the wrong field.
static_assert(offsetof(WifiSettingsPersistent, Enabled) == 0, "Enabled offset");
static_assert(offsetof(WifiSettingsPersistent, Ssid) == 4, "SSID offset");
static_assert(offsetof(WifiSettingsVolatile, Password) == 0, "Password offset");
static_assert(offsetof(WifiSettingsVolatile, State) == WIFI_PASSWORD_LEN, "State offset");
static_assert(offsetof(WifiSettingsVolatile, Ip) == WIFI_PASSWORD_LEN + 4, "IP address offset");

// The State a device WITHOUT a radio can report, from the configuration alone. `HasSsid` is "the
// SSID holds at least one character" and `HasPassword` is the cached presence of the NVS entry -
// neither is a connection result, which is why this can answer 3 and 4 with no radio behind it.
//
// Only 0, 3 and 4 are reachable (see the header note): 1 Connecting, 2 Connected, 5 Wrong password
// and 6 Error all describe what a radio did, and this build has none.
inline uint8_t WifiStateFrom(bool Enabled, bool HasSsid, bool HasPassword)
{
    if (!Enabled)
        return WIFI_STATE_DISCONNECTED; // the radio is switched off; no state value describes that
    if (!HasSsid)
        return WIFI_STATE_NO_SSID;
    if (!HasPassword)
        return WIFI_STATE_NO_PASSWORD;
    return WIFI_STATE_DISCONNECTED; // configured, and not associated because nothing associates
}

// True when a fixed-width character field holds at least one character. The field is a fixed-size
// record with no terminator of its own (Text is NUL-padded), so "empty" is "every byte is NUL";
// this is the rule `State = 3 No SSID` is decided on, and it is the same scan the app does.
inline bool WifiTextPresent(const char *Text, uint16_t Len)
{
    for (uint16_t i = 0; i < Len; i++)
        if (Text[i] != '\0')
            return true;
    return false;
}

// Defined in the board glue: the write side of the block (Devices/Tamu_v2.0A/WifiSettingsGlue.h).
bool OnWifiEnabledWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnWifiSsidWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnWifiPasswordWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);

const BlockEntry WifiSettings_Entries[] = {
    // Settable and persistent, and both go through a trigger: the whole block is closed to the
    // radio, not just its secret, so a plain copy path would bypass that rule.
    { MakeFieldKey(WIFI_SET_FIELD_ENABLED, 0), 0,
      { (uint16_t)DataType::Bool, sizeof(uint8_t), (uint8_t)(ValuePersistent | ValueTrigger) } },
    { MakeFieldKey(WIFI_SET_FIELD_SSID, 0), 4,
      { (uint16_t)DataType::Text, WIFI_SSID_LEN, (uint8_t)(ValuePersistent | ValueTrigger) } },
    // The password: NOT persistent (NVS) and write-only (reads answer the always-empty volatile
    // buffer). The trigger is what keeps the text out of the register file.
    { MakeFieldKey(WIFI_SET_FIELD_PASSWORD, 0), 0,
      { (uint16_t)DataType::Text, WIFI_PASSWORD_LEN, (uint8_t)ValueTrigger } },
    // Read-only reports of the link's state.
    { MakeFieldKey(WIFI_SET_FIELD_STATE, 0), WIFI_PASSWORD_LEN,
      { (uint16_t)DataType::Enum, sizeof(uint8_t), ValueReadOnly } },
    // 16 raw bytes: the address in network order (IPv6, or IPv4 IPv4-mapped). All-zero = none.
    { MakeFieldKey(WIFI_SET_FIELD_IP, 0), WIFI_PASSWORD_LEN + 4,
      { (uint16_t)DataType::Undefined, WIFI_IP_LEN, ValueReadOnly } },
};

const BlockTrigger WifiSettings_Triggers[] = {
    { MakeFieldKey(WIFI_SET_FIELD_ENABLED, 0), OnWifiEnabledWrite },
    { MakeFieldKey(WIFI_SET_FIELD_SSID, 0), OnWifiSsidWrite },
    { MakeFieldKey(WIFI_SET_FIELD_PASSWORD, 0), OnWifiPasswordWrite },
};

const BlockSchema WifiSettingsSchema = {
    .Entries = WifiSettings_Entries,
    .EntryCount = sizeof(WifiSettings_Entries) / sizeof(BlockEntry),
    .Triggers = WifiSettings_Triggers,
    .TriggerCount = sizeof(WifiSettings_Triggers) / sizeof(BlockTrigger),
    .Type = BlockType::WifiSettings,
};
