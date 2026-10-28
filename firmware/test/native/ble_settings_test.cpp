// Host-side test for the BLE Settings block and the link-security policy it feeds
// (Blocks/BleSettings.h + Blocks/BleSecurityPolicy.h).
//
// Two things are checked here, both without a radio:
//
//  1. The policy model. The same function the device uses (BleSecurityPolicyFrom) turns the
//     block's Passkey text into the pairing configuration AND the GATT attribute flags, so the
//     Just Works / Passkey switch and "every permission requires encryption" are decided in one
//     place. The invariants below are what makes the switch safe: a writable or notifying
//     attribute never appears without its *_ENC twin, and the *_AUTHEN bits appear exactly when a
//     passkey is set.
//
//  2. The block's entry table against the struct layout. The entries address the two value spaces
//     by byte offset, so a field added or reordered without the table (or the reverse) silently
//     serves the wrong bytes. Driving the real StaticBlockDescriptor through Get/Set here pins the
//     offsets, the types, the Read Only refusals and the two triggers.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Blocks/BleSettings.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                            \
    } while (0)

// The block's triggers live in the board glue (Devices/Tamu_v2.0A/BleSettingsGlue.h), which needs
// the radio. Defining them here lets the table be driven and records that a write reached the
// trigger instead of the generic copy.
static int PasskeyTriggerCalls = 0;
static int ActionTriggerCalls = 0;
static int EnabledTriggerCalls = 0;
static int DisplayNameTriggerCalls = 0;
static char PasskeyTriggerText[BLE_PASSKEY_LEN + 1] = {0};
static uint8_t ActionTriggerValue = 0xFF;

bool OnBlePasskeyWrite(const StaticBlockDescriptor &, uint16_t, const void *Data, uint16_t Length)
{
    PasskeyTriggerCalls++;
    if (Length != BLE_PASSKEY_LEN)
        return false;
    memcpy(PasskeyTriggerText, Data, BLE_PASSKEY_LEN);
    PasskeyTriggerText[BLE_PASSKEY_LEN] = '\0';
    return true;
}

// The block's Display name write is USB-only in the glue; the stub copies it so the table's wiring
// can be checked.
bool OnBleDisplayNameWrite(const StaticBlockDescriptor &Block, uint16_t, const void *Data, uint16_t Length)
{
    DisplayNameTriggerCalls++;
    if (Length != BLE_DISPLAY_NAME_LEN)
        return false;
    memcpy(((BleSettingsPersistent *)Block.PersistentData)->DisplayName, Data, BLE_DISPLAY_NAME_LEN);
    return true;
}

bool OnBleActionWrite(const StaticBlockDescriptor &, uint16_t, const void *Data, uint16_t Length)
{
    ActionTriggerCalls++;
    if (Length != 1)
        return false;
    ActionTriggerValue = *(const uint8_t *)Data;
    return true;
}

// The block's Enabled field routes through a trigger (the real one refuses a non-USB write); the
// stub applies the value so the table's wiring can be checked.
bool OnBleEnabledWrite(const StaticBlockDescriptor &Block, uint16_t, const void *Data, uint16_t Length)
{
    EnabledTriggerCalls++;
    if (Length != 1)
        return false;
    ((BleSettingsPersistent *)Block.PersistentData)->Enabled = *(const uint8_t *)Data ? 1 : 0;
    return true;
}

static void TestPolicy()
{
    // --- the text forms ---
    CHECK(BlePasskeyDigits("000000"));
    CHECK(BlePasskeyDigits("123456"));
    CHECK(!BlePasskeyDigits("12345x"));   // not a digit
    CHECK(!BlePasskeyDigits(" 12345"));   // space
    CHECK(!BlePasskeyDigits("12345:"));   // ':' is the character after '9'

    // `000000` is the disabled sentinel, not a passkey of zero: 0 is not a legal SM passkey.
    CHECK(!BlePasskeyEnabled("000000"));
    CHECK(BlePasskeyEnabled("000001"));
    CHECK(BlePasskeyEnabled("999999"));
    CHECK(!BlePasskeyEnabled("12a456"));

    CHECK(BlePasskeyValue("000000") == 0);
    CHECK(BlePasskeyValue("000123") == 123);
    CHECK(BlePasskeyValue("456789") == 456789);

    // --- A: Just Works, the default ---
    {
        BleSecurityPolicy P = BleSecurityPolicyFrom("000000");
        CHECK(P.Passkey == 0);
        CHECK(P.Mitm == false);
        CHECK(P.IoCap == BleIoCapNoInputOutput);   // no input, no output: Just Works
        CHECK(P.RxFlags == (BleAttWrite | BleAttWriteNoResponse | BleAttWriteEncrypted));
        CHECK((P.RxFlags & BleAttWriteAuthenticated) == 0); // nothing to authenticate without a code
        CHECK(P.TxFlags == (BleAttNotify | BleAttNotifyEncrypted));
    }

    // --- B: a passkey is set -> MITM, the device shows the code ---
    {
        BleSecurityPolicy P = BleSecurityPolicyFrom("123456");
        CHECK(P.Passkey == 123456);
        CHECK(P.Mitm == true);
        CHECK(P.IoCap == BleIoCapDisplayOnly);
        CHECK(P.RxFlags == (BleAttWrite | BleAttWriteNoResponse | BleAttWriteEncrypted |
                            BleAttWriteAuthenticated));
        CHECK(P.TxFlags == (BleAttNotify | BleAttNotifyEncrypted));
    }

    // --- the invariants every policy shape must satisfy ---
    const char *Texts[] = {"000000", "000001", "100000", "654321", "999999", "12345x", ""};
    for (const char *Text : Texts)
    {
        BleSecurityPolicy P = BleSecurityPolicyFrom(Text);

        // Every writable/notifying permission carries its encryption requirement.
        if (P.RxFlags & (BleAttWrite | BleAttWriteNoResponse))
            CHECK(P.RxFlags & BleAttWriteEncrypted);
        if (P.TxFlags & BleAttNotify)
            CHECK(P.TxFlags & BleAttNotifyEncrypted);

        // ... and the AUTHEN bit exists exactly when a passkey is set (the *_ENC of a notify
        // attribute already excludes an unauthenticated peer, see the header's note).
        CHECK(((P.RxFlags & BleAttWriteAuthenticated) != 0) == P.Mitm);

        // The passkey and the mode can never disagree: one is derived from the other.
        CHECK((P.Passkey != 0) == P.Mitm);
        CHECK(P.Mitm == BlePasskeyEnabled(Text));
    }

    // A malformed text is not a passkey: it must fall back to Just Works rather than to some
    // half-set state (the block's trigger refuses such a write, this is the second line).
    {
        BleSecurityPolicy P = BleSecurityPolicyFrom("12a456");
        CHECK(!P.Mitm);
        CHECK(P.Passkey == 0);
        CHECK(P.IoCap == BleIoCapNoInputOutput);
    }
}

static void TestBlockTable()
{
    BleSettingsPersistent Per;
    BleSettingsVolatile Vol;
    StaticBlockDescriptor Blk = {&Vol, &Per, &BleSettingsSchema, "BLE Settings", nullptr};

    // --- the fields resolve to the struct members the offsets claim ---
    // The passkey is readable and writable, but NOT persistent: it lives in NVS, so the register
    // file (.SV) never carries the secret. Its wire copy is the volatile field.
    FieldResult R = Blk.Get(BLE_SET_FIELD_PASSKEY, 0);
    CHECK(R.Data == &Vol.Passkey);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Text);
    CHECK(R.Descriptor.Size == BLE_PASSKEY_LEN);
    CHECK(!ValueIsPersistent(R.Descriptor));
    CHECK(!ValueIsReadOnly(R.Descriptor)); // readable back: the app may show the user their code

    R = Blk.Get(BLE_SET_FIELD_MODE, 0);
    CHECK(R.Data == &Vol.Mode);
    CHECK(ValueIsReadOnly(R.Descriptor));

    R = Blk.Get(BLE_SET_FIELD_BONDCOUNT, 0);
    CHECK(R.Data == &Vol.BondCount);
    CHECK(R.Descriptor.Size == 1);

    // The Bond field is keyed: each key is its own buffer, not three names for one.
    for (uint8_t k = 0; k < BLE_SETTINGS_BOND_SLOTS; k++)
    {
        R = Blk.Get(BLE_SET_FIELD_BOND, k);
        CHECK(R.Data == &Vol.Bond[k][0]);
        CHECK(R.Descriptor.Size == BLE_BOND_TEXT_LEN);
        CHECK(ValueIsReadOnly(R.Descriptor));
    }
    CHECK(Blk.Get(BLE_SET_FIELD_BOND, BLE_SETTINGS_BOND_SLOTS).Data == nullptr); // no hidden slot

    R = Blk.Get(BLE_SET_FIELD_WINDOW, 0);
    CHECK(R.Data == &Vol.WindowMs);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Uint32);
    CHECK(R.Descriptor.Size == sizeof(uint32_t));

    R = Blk.Get(BLE_SET_FIELD_ACTION, 0);
    CHECK(R.Data == &Vol.Action);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Enum);
    CHECK(!ValueIsReadOnly(R.Descriptor)); // a write is the command

    R = Blk.Get(BLE_SET_FIELD_ENABLED, 0);
    CHECK(R.Data == &Per.Enabled);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Bool);
    CHECK(R.Descriptor.Size == 1);
    CHECK(ValueIsPersistent(R.Descriptor));

    R = Blk.Get(BLE_SET_FIELD_DISPLAYNAME, 0);
    CHECK(R.Data == &Per.DisplayName);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Text);
    CHECK(R.Descriptor.Size == BLE_DISPLAY_NAME_LEN);
    CHECK(ValueIsPersistent(R.Descriptor));

    // The resolved advertised name is a read-out of the rule "empty Display name = System.Name".
    R = Blk.Get(BLE_SET_FIELD_ADVERTISED, 0);
    CHECK(R.Data == &Vol.AdvertisedName);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Text);
    CHECK(R.Descriptor.Size == BLE_DISPLAY_NAME_LEN);
    CHECK(ValueIsReadOnly(R.Descriptor));

    // The E66 read-out: the identity the admission used, the address the peer came from, and the
    // verdict. All three are read-only and in the volatile space (nothing of a link is persisted),
    // and the two addresses are the same width as a Bond text so a rig can read them all the same way.
    R = Blk.Get(BLE_SET_FIELD_LASTPEER, 0);
    CHECK(R.Data == &Vol.LastPeer);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Text);
    CHECK(R.Descriptor.Size == BLE_BOND_TEXT_LEN);
    CHECK(ValueIsReadOnly(R.Descriptor));
    CHECK(!ValueIsPersistent(R.Descriptor));

    R = Blk.Get(BLE_SET_FIELD_LASTAIR, 0);
    CHECK(R.Data == &Vol.LastAir);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Text);
    CHECK(R.Descriptor.Size == BLE_BOND_TEXT_LEN);
    CHECK(ValueIsReadOnly(R.Descriptor));
    CHECK(!ValueIsPersistent(R.Descriptor));

    R = Blk.Get(BLE_SET_FIELD_LASTLINK, 0);
    CHECK(R.Data == &Vol.LastLink);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Enum);
    CHECK(R.Descriptor.Size == 1);
    CHECK(ValueIsReadOnly(R.Descriptor));

    // A fresh device has judged no link: both addresses read empty and the verdict says so, so a
    // rig cannot mistake "nothing connected yet" for "connected and refused".
    CHECK(Vol.LastPeer[0] == '\0');
    CHECK(Vol.LastAir[0] == '\0');
    CHECK(Vol.LastLink == BleLinkNone);

    // --- the defaults a device with a zero .SV tail gets ---
    CHECK(Per.Enabled == 1);                        // radio on out of the box
    CHECK(memcmp(Per.ScrubbedPasskey, "000000", BLE_PASSKEY_LEN) == 0); // nothing inherited
    CHECK(memcmp(Vol.Passkey, "000000", BLE_PASSKEY_LEN) == 0);         // Just Works
    CHECK(Per.DisplayName[0] == '\0');              // empty = use System.Name
    CHECK(Vol.Mode == BLE_SET_MODE_JUST_WORKS);
    CHECK(Vol.WindowMs == 0);                       // closed

    // --- Read Only is refused, not silently ignored ---
    uint8_t One = 1;
    ValueInfo BoolDesc = {(uint16_t)DataType::Bool, 1, 0};
    CHECK(!Blk.Set(BLE_SET_FIELD_MODE, 0, &One, 1, BoolDesc));
    ValueInfo U32Desc = {(uint16_t)DataType::Uint32, 4, 0};
    uint32_t Zero = 0;
    CHECK(!Blk.Set(BLE_SET_FIELD_WINDOW, 0, &Zero, 4, U32Desc));
    ValueInfo TextDesc = {(uint16_t)DataType::Text, BLE_BOND_TEXT_LEN, 0};
    CHECK(!Blk.Set(BLE_SET_FIELD_BOND, 0, "X", 1, TextDesc));

    // --- the settable fields ---
    // Display name reaches its trigger (the whole block is USB-only, so a plain copy path would
    // bypass the rule)...
    ValueInfo NameDesc = {(uint16_t)DataType::Text, BLE_DISPLAY_NAME_LEN, 0};
    CHECK(Blk.Set(BLE_SET_FIELD_DISPLAYNAME, 0, "Tamu desk", 9, NameDesc));
    CHECK(DisplayNameTriggerCalls == 1);
    CHECK(memcmp(Per.DisplayName, "Tamu desk", 9) == 0);
    CHECK(Per.DisplayName[9] == '\0');   // null-padded to the field width
    CHECK(Per.DisplayName[15] == '\0');

    // ...Enabled belongs to its trigger as well.
    uint8_t Off = 0;
    CHECK(Blk.Set(BLE_SET_FIELD_ENABLED, 0, &Off, 1, BoolDesc));
    CHECK(EnabledTriggerCalls == 1);
    CHECK(Per.Enabled == 0);
    CHECK(PasskeyTriggerCalls == 0); // and the passkey trigger was not involved

    // --- the passkey write belongs to the trigger, not to the copy path ---
    ValueInfo PassDesc = {(uint16_t)DataType::Text, BLE_PASSKEY_LEN, 0};
    CHECK(Blk.Set(BLE_SET_FIELD_PASSKEY, 0, "654321", BLE_PASSKEY_LEN, PassDesc));
    CHECK(PasskeyTriggerCalls == 1);
    CHECK(strcmp(PasskeyTriggerText, "654321") == 0);
    CHECK(memcmp(Vol.Passkey, "654321", BLE_PASSKEY_LEN) != 0); // the trigger applies it, not Set()
    // A shorter value is null-padded to the field width and reaches the trigger that way, so the
    // glue's "not six digits" refusal is what stops it - not a silent truncation here.
    CHECK(Blk.Set(BLE_SET_FIELD_PASSKEY, 0, "12", 2, PassDesc));
    CHECK(PasskeyTriggerCalls == 2);
    CHECK(strlen(PasskeyTriggerText) == 2);

    // --- the action write reaches its trigger with the command byte ---
    // The command field is an Enum of one byte: a Bool-typed write is refused by the type check
    // above the trigger, which is why the app writes it as an Enum.
    ValueInfo EnumDesc = {(uint16_t)DataType::Enum, 1, 0};
    uint8_t Open = BLE_SET_ACTION_OPEN_WINDOW;
    CHECK(!Blk.Set(BLE_SET_FIELD_ACTION, 0, &Open, 1, BoolDesc)); // wrong type: never reaches the trigger
    CHECK(ActionTriggerCalls == 0);
    CHECK(Blk.Set(BLE_SET_FIELD_ACTION, 0, &Open, 1, EnumDesc));
    CHECK(ActionTriggerCalls == 1);
    CHECK(ActionTriggerValue == BLE_SET_ACTION_OPEN_WINDOW);
    uint8_t Forget = BLE_SET_ACTION_FORGET_BONDS;
    CHECK(Blk.Set(BLE_SET_FIELD_ACTION, 0, &Forget, 1, EnumDesc));
    CHECK(ActionTriggerValue == BLE_SET_ACTION_FORGET_BONDS);

    // A multi-byte value on the command field is refused by the type/size check before the
    // trigger: the command is one byte.
    uint16_t Wide = 0x0100;
    ValueInfo WideDesc = {(uint16_t)DataType::Enum, 2, 0};
    CHECK(!Blk.Set(BLE_SET_FIELD_ACTION, 0, &Wide, 2, WideDesc));
    CHECK(ActionTriggerCalls == 2);
}

// --- the Bond read-out's refresh decision -------------------------------------
//
// The block's Bond texts are a cache of the NimBLE bond store. Refreshing it on the store's SIZE
// was wrong: a fourth phone makes the stack evict the oldest bond and add the new one inside a
// single store callback, so getNumBonds() reads 3 before and 3 after. This drives the predicate the
// glue actually uses, so that case is the first thing checked.
static void TestBondRefresh()
{
    // Boot: nothing formatted yet, the first pass must always run.
    BleBondRefresh S;
    CHECK(BleBondRefreshDue(S, 0, 0, 0));

    // A pass just ran at t=0 with 3 bonds, generation 7: nothing is due again immediately.
    S.Generation = 7;
    S.Count = 3;
    S.DueMs = 0 + 1000;
    CHECK(!BleBondRefreshDue(S, 7, 3, 1));
    CHECK(!BleBondRefreshDue(S, 7, 3, 999));

    // THE EVICTION CASE: a fourth phone pairs, the oldest bond is evicted and the new one stored in
    // the same callback - the count is unchanged, only the content moved. The generation is what
    // catches it, on the very next pass.
    CHECK(BleBondRefreshDue(S, 8, 3, 1));

    // A count change alone still refreshes, so a store that emptied is reported even if the
    // generation counter were never bumped (radio switched off, bonds gone).
    CHECK(BleBondRefreshDue(S, 7, 2, 1));

    // The slow content check: due once the period has elapsed, with neither of the other two keys
    // (a phone that removed its own bond, a failed encryption the stack unpaired on).
    CHECK(BleBondRefreshDue(S, 7, 3, 1000));
    CHECK(BleBondRefreshDue(S, 7, 3, 1001));

    // TimeFromBoot() is a uint32 counter that wraps after ~49 days; the comparison is signed so a
    // wrap does not stall the check for the rest of the uptime.
    S.DueMs = 0xFFFFFFFFu; // due "4.29 s of counter" from the wrap
    CHECK(!BleBondRefreshDue(S, 7, 3, 0xFFFFFF00u));
    CHECK(BleBondRefreshDue(S, 7, 3, 0x00000200u)); // wrapped past the due point
}

// --- how long a link may stay unsecured ---------------------------------------
//
// The deadline the device grants a fresh link is selected by the policy (AppBLE.h's onConnect asks
// AppBLETick for it through BleSecureBudgetMs). The unit that matters: Just Works has one tap to
// wait for, a passkey has a person reading a prompt and typing six digits, and one short figure for
// both drops every passkey pairing mid-procedure - the failure that looks like a wrong code.
static void TestSecureBudget()
{
    const BleSecurityPolicy JustWorks = BleSecurityPolicyFrom("000000");
    const BleSecurityPolicy Passkey = BleSecurityPolicyFrom("123456");

    CHECK(BleSecureBudgetMs(JustWorks) == BleSecureBudgetJustWorksMs);
    CHECK(BleSecureBudgetMs(Passkey) == BleSecureBudgetPasskeyMs);

    // The passkey budget has to be the generous one: it covers a human step ...
    CHECK(BleSecureBudgetPasskeyMs > BleSecureBudgetJustWorksMs);
    CHECK(BleSecureBudgetPasskeyMs >= 20000);

    // ... and it still has to fit inside the 60 s connectable window (BLE_WINDOW_MS in AppBLE.h,
    // which static_asserts the pair stays ordered), so one window always holds the whole procedure.
    CHECK(BleSecureBudgetPasskeyMs < 60000);
    CHECK(BleSecureBudgetJustWorksMs <= 60000);

    // Any policy shape resolves, and a malformed text is Just Works (never a third budget).
    const char *Texts[] = {"000000", "000001", "999999", "12a456", ""};
    for (const char *Text : Texts)
    {
        const BleSecurityPolicy P = BleSecurityPolicyFrom(Text);
        CHECK(BleSecureBudgetMs(P) == (P.Mitm ? BleSecureBudgetPasskeyMs : BleSecureBudgetJustWorksMs));
    }
}

int main()
{
    TestPolicy();
    TestBlockTable();
    TestBondRefresh();
    TestSecureBudget();

    printf("ble_settings: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
