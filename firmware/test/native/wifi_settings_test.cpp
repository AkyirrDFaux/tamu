// Host-side test for the WiFi Settings block (Blocks/WifiSettings.h) and the System field 8 bit
// field it sits beside (Core/Types/Enums.h AppActiveBits).
//
// Three things are checked here, all without a radio:
//
//  1. The block's entry table against the struct layout. The entries address the two value spaces by
//     byte offset, so a field added or reordered without the table (or the reverse) silently serves
//     the wrong bytes. Driving the real StaticBlockDescriptor through Get/Set pins the offsets, the
//     types, the Read Only refusals and the three triggers.
//
//  2. The password is WRITE-ONLY. Its entry is a Trigger into the volatile space and the trigger (the
//     board glue's, stubbed below exactly as the real one behaves) never copies the text back, so a
//     read of the field answers 64 zero bytes. If a future change made it a plain field, or filled
//     the buffer "for convenience", this is what fails.
//
//  3. The reported State. With no radio implemented, `WifiStateFrom` may only answer 0 Disconnected,
//     3 No SSID and 4 No password - a configuration check - and never 1 Connecting, 2 Connected,
//     5 Wrong password or 6 Error, which describe what a radio did.
//
// No device dependency: built like align/stride/blesettings (see run.sh).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Blocks/WifiSettings.h"
#include "Core/Types/Enums.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
        }                                                                            \
    } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__,      \
                   #got, #want, g, w);                                               \
        }                                                                            \
    } while (0)

// The block's triggers live in the board glue, which needs NVS and the app link. These stubs behave
// the way the real ones do in the two respects this test can see: the value is applied to the
// persistent space (Enabled, SSID), and the password NEVER reaches the volatile buffer.
static int EnabledTriggerCalls = 0;
static int SsidTriggerCalls = 0;
static int PasswordTriggerCalls = 0;
static bool PasswordStored = false; // the glue's cached presence flag, in place of NVS
static char PasswordSeen[WIFI_PASSWORD_LEN + 1] = {0};

bool OnWifiEnabledWrite(const StaticBlockDescriptor &Block, uint16_t, const void *Data, uint16_t Length)
{
    EnabledTriggerCalls++;
    if (Length != 1)
        return false;
    ((WifiSettingsPersistent *)Block.PersistentData)->Enabled = *(const uint8_t *)Data ? 1 : 0;
    return true;
}

bool OnWifiSsidWrite(const StaticBlockDescriptor &Block, uint16_t, const void *Data, uint16_t Length)
{
    SsidTriggerCalls++;
    if (Length != WIFI_SSID_LEN)
        return false;
    memcpy(((WifiSettingsPersistent *)Block.PersistentData)->Ssid, Data, WIFI_SSID_LEN);
    return true;
}

bool OnWifiPasswordWrite(const StaticBlockDescriptor &, uint16_t, const void *Data, uint16_t Length)
{
    PasswordTriggerCalls++;
    if (Length != WIFI_PASSWORD_LEN)
        return false;
    const char *Text = (const char *)Data;
    uint16_t Used = 0;
    while (Used < WIFI_PASSWORD_LEN && Text[Used] != '\0')
        Used++;
    PasswordStored = Used != 0; // NVS save/erase, whose presence flag the glue caches
    memcpy(PasswordSeen, Text, Used);
    PasswordSeen[Used] = '\0';
    return true;
}

static void TestBlockTable()
{
    WifiSettingsPersistent Per;
    WifiSettingsVolatile Vol;
    StaticBlockDescriptor Blk = {&Vol, &Per, &WifiSettingsSchema, "WiFi Settings", nullptr};

    // --- the block's identity: type 0x0C, five fields, three triggers ---
    CHECK_EQ((int)WifiSettingsSchema.Type, 0x0C);
    CHECK_EQ((int)WifiSettingsSchema.EntryCount, 5);
    CHECK_EQ((int)WifiSettingsSchema.TriggerCount, 3);

    // --- every field resolves to the struct member its offset claims, with its documented shape ---
    FieldResult R = Blk.Get(WIFI_SET_FIELD_ENABLED, 0);
    CHECK(R.Data == &Per.Enabled);
    CHECK_EQ(ValueInfoType(R.Descriptor), (uint16_t)DataType::Bool);
    CHECK_EQ(R.Descriptor.Size, 1);
    CHECK(ValueIsPersistent(R.Descriptor));   // settings survive a reboot (.SV)
    CHECK(!ValueIsReadOnly(R.Descriptor));

    R = Blk.Get(WIFI_SET_FIELD_SSID, 0);
    CHECK(R.Data == &Per.Ssid[0]);
    CHECK_EQ(ValueInfoType(R.Descriptor), (uint16_t)DataType::Text);
    CHECK_EQ(R.Descriptor.Size, WIFI_SSID_LEN);
    CHECK_EQ(WIFI_SSID_LEN, 32); // the 802.11 limit, and the app's maxChars
    CHECK(ValueIsPersistent(R.Descriptor));
    CHECK(!ValueIsReadOnly(R.Descriptor));

    // The password: readable on the wire (the field exists) but NOT persistent - the text lives in
    // NVS, so `.SV` - a storage-partition file - never carries the credential.
    R = Blk.Get(WIFI_SET_FIELD_PASSWORD, 0);
    CHECK(R.Data == &Vol.Password[0]);
    CHECK_EQ(ValueInfoType(R.Descriptor), (uint16_t)DataType::Text);
    CHECK_EQ(R.Descriptor.Size, WIFI_PASSWORD_LEN);
    CHECK(!ValueIsPersistent(R.Descriptor));
    CHECK(!ValueIsReadOnly(R.Descriptor));

    R = Blk.Get(WIFI_SET_FIELD_STATE, 0);
    CHECK(R.Data == &Vol.State);
    CHECK_EQ(ValueInfoType(R.Descriptor), (uint16_t)DataType::Enum);
    CHECK_EQ(R.Descriptor.Size, 1);
    CHECK(ValueIsReadOnly(R.Descriptor));

    // The address: 16 raw bytes, network order, read only. Undefined is the point - the device never
    // says what family the bytes are, the app decodes them (IPv6, or IPv4 IPv4-mapped).
    R = Blk.Get(WIFI_SET_FIELD_IP, 0);
    CHECK(R.Data == &Vol.Ip[0]);
    CHECK_EQ(ValueInfoType(R.Descriptor), (uint16_t)DataType::Undefined);
    CHECK_EQ(R.Descriptor.Size, 16);
    CHECK(ValueIsReadOnly(R.Descriptor));

    // --- the defaults a device that never configured WiFi has ---
    CHECK_EQ(Per.Enabled, 0);                                   // off by default
    CHECK(!WifiTextPresent(Per.Ssid, WIFI_SSID_LEN));           // no network
    CHECK_EQ(Vol.State, WIFI_STATE_DISCONNECTED);               // and nothing to report
    for (int i = 0; i < WIFI_IP_LEN; i++) CHECK_EQ(Vol.Ip[i], 0); // no address (all-zero)

    // --- Read Only is refused, not silently ignored ---
    uint8_t One = 1;
    uint8_t Zero = 0;
    ValueInfo BoolDesc = {(uint16_t)DataType::Bool, 1, 0};
    ValueInfo EnumDesc = {(uint16_t)DataType::Enum, 1, 0};
    ValueInfo IpDesc = {(uint16_t)DataType::Undefined, 16, 0};
    uint8_t Ip[16] = {0};
    CHECK(!Blk.Set(WIFI_SET_FIELD_STATE, 0, &One, 1, EnumDesc));
    CHECK(!Blk.Set(WIFI_SET_FIELD_IP, 0, Ip, 16, IpDesc));
    CHECK(!Blk.Set(WIFI_SET_FIELD_STATE, 0, &One, 1, BoolDesc)); // wrong type as well

    // --- the settable fields, through their triggers (the block is USB-only, so a plain copy path
    //     would bypass the rule) ---
    CHECK(Blk.Set(WIFI_SET_FIELD_ENABLED, 0, &One, 1, BoolDesc));
    CHECK_EQ(EnabledTriggerCalls, 1);
    CHECK_EQ(Per.Enabled, 1);
    CHECK_EQ(SsidTriggerCalls, 0); // and no other trigger was involved
    CHECK(Blk.Set(WIFI_SET_FIELD_ENABLED, 0, &Zero, 1, BoolDesc));
    CHECK_EQ(Per.Enabled, 0);

    ValueInfo SsidDesc = {(uint16_t)DataType::Text, WIFI_SSID_LEN, 0};
    CHECK(Blk.Set(WIFI_SET_FIELD_SSID, 0, "TamuNet", 7, SsidDesc));
    CHECK_EQ(SsidTriggerCalls, 1);
    CHECK(memcmp(Per.Ssid, "TamuNet", 7) == 0);
    CHECK_EQ(Per.Ssid[7], 0);  // null-padded to the field width by the register path
    CHECK_EQ(Per.Ssid[31], 0);
    CHECK(WifiTextPresent(Per.Ssid, WIFI_SSID_LEN));

    // Clearing the network: an all-NUL write is "no SSID", which is what State 3 reports.
    memset(Per.Ssid, 0, WIFI_SSID_LEN);
    CHECK(!WifiTextPresent(Per.Ssid, WIFI_SSID_LEN));

    // --- the password: write-only ---
    ValueInfo PassDesc = {(uint16_t)DataType::Text, WIFI_PASSWORD_LEN, 0};
    CHECK(Blk.Set(WIFI_SET_FIELD_PASSWORD, 0, "hunter2", 7, PassDesc));
    CHECK_EQ(PasswordTriggerCalls, 1);
    CHECK(strcmp(PasswordSeen, "hunter2") == 0); // the trigger received it (padded to 64)
    CHECK(PasswordStored);                       // ... and recorded its presence
    // ... and the register file holds NOTHING: the volatile buffer is still all zeros.
    for (int i = 0; i < WIFI_PASSWORD_LEN; i++) CHECK_EQ(Vol.Password[i], 0);
    R = Blk.Get(WIFI_SET_FIELD_PASSWORD, 0);
    CHECK(R.Data == &Vol.Password[0]);
    for (int i = 0; i < R.Descriptor.Size; i++)
        CHECK_EQ(((const uint8_t *)R.Data)[i], 0); // a read answers empty, never the text

    // An empty write is the forget path (the real trigger erases the NVS entry).
    memset(PasswordSeen, 'x', WIFI_PASSWORD_LEN);
    PasswordSeen[WIFI_PASSWORD_LEN] = '\0';
    CHECK(Blk.Set(WIFI_SET_FIELD_PASSWORD, 0, "", 0, PassDesc));
    CHECK_EQ(PasswordTriggerCalls, 2);
    CHECK(!PasswordStored);
}

static void TestState()
{
    // The whole truth table. Only the three configuration states are reachable without a radio.
    CHECK_EQ(WifiStateFrom(false, false, false), WIFI_STATE_DISCONNECTED); // radio off
    CHECK_EQ(WifiStateFrom(false, true, true), WIFI_STATE_DISCONNECTED);   // radio off, network saved
    CHECK_EQ(WifiStateFrom(true, false, false), WIFI_STATE_NO_SSID);
    CHECK_EQ(WifiStateFrom(true, false, true), WIFI_STATE_NO_SSID);        // no SSID outranks no password
    CHECK_EQ(WifiStateFrom(true, true, false), WIFI_STATE_NO_PASSWORD);
    CHECK_EQ(WifiStateFrom(true, true, true), WIFI_STATE_DISCONNECTED);    // configured, unassociated

    for (int Enabled = 0; Enabled <= 1; Enabled++)
        for (int Ssid = 0; Ssid <= 1; Ssid++)
            for (int Pass = 0; Pass <= 1; Pass++)
            {
                const uint8_t S = WifiStateFrom(Enabled != 0, Ssid != 0, Pass != 0);
                // What a configuration check can prove, and nothing else: 1 Connecting, 2 Connected,
                // 5 Wrong password and 6 Error all describe a radio this build does not have.
                CHECK(S == WIFI_STATE_DISCONNECTED || S == WIFI_STATE_NO_SSID || S == WIFI_STATE_NO_PASSWORD);
                CHECK(S != WIFI_STATE_CONNECTING);
                CHECK(S != WIFI_STATE_CONNECTED);
                CHECK(S != WIFI_STATE_WRONG_PASSWORD);
                CHECK(S != WIFI_STATE_ERROR);
            }

    // The numbering is the app's (app/lib/core/wifi_settings.dart) - it labels these values.
    CHECK_EQ(WIFI_STATE_DISCONNECTED, 0);
    CHECK_EQ(WIFI_STATE_CONNECTING, 1);
    CHECK_EQ(WIFI_STATE_CONNECTED, 2);
    CHECK_EQ(WIFI_STATE_NO_SSID, 3);
    CHECK_EQ(WIFI_STATE_NO_PASSWORD, 4);
    CHECK_EQ(WIFI_STATE_WRONG_PASSWORD, 5);
    CHECK_EQ(WIFI_STATE_ERROR, 6);
}

static void TestAppActiveBits()
{
    // System field 8 is a bit field: one bit per live interface, so both links can be reported at
    // once. The app labels them (app/lib/core/app_active.dart), in the same numbering.
    CHECK_EQ(AppActiveBits::USB, 1);
    CHECK_EQ(AppActiveBits::BLE, 2);
    CHECK_EQ(AppActiveBits::WiFi, 4);

    CHECK_EQ(AppActiveBits::Of(false, false), 0); // no app
    CHECK_EQ(AppActiveBits::Of(true, false), 1);  // what the old enum called USB
    CHECK_EQ(AppActiveBits::Of(false, true), 2);  // what the old enum called BLE - an old client still
                                                  // decodes these two as the small integers it knows
    CHECK_EQ(AppActiveBits::Of(true, true), 3);   // the value the old enum could not express at all

    // Bit 2 is reserved and never set: the WiFi app transport is not implemented, so there is
    // nothing that could raise it.
    for (int Usb = 0; Usb <= 1; Usb++)
        for (int Ble = 0; Ble <= 1; Ble++)
        {
            const uint8_t Bits = AppActiveBits::Of(Usb != 0, Ble != 0);
            CHECK_EQ(Bits & AppActiveBits::WiFi, 0);
            CHECK(Bits <= 3); // the field is one byte, and only these two bits are produced
        }
}

int main()
{
    TestBlockTable();
    TestState();
    TestAppActiveBits();

    printf("wifi_settings: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
