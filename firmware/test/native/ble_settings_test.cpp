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
static int EnabledTriggerCalls = 0;
static int DisplayNameTriggerCalls = 0;
static int BondsTriggerCalls = 0;
static int WindowTriggerCalls = 0;
static char PasskeyTriggerText[BLE_PASSKEY_LEN + 1] = {0};
static uint8_t BondsTriggerValue[BLE_BOND_ARRAY_LEN] = {0};
static uint32_t WindowTriggerValue = 0xFFFFFFFFu;

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

// The Bonds write is a whole-array value the glue checks against the bond store; here the stub only
// records the bytes it was handed, so the table can be checked for routing and size.
bool OnBleBondsWrite(const StaticBlockDescriptor &, uint16_t, const void *Data, uint16_t Length)
{
    BondsTriggerCalls++;
    if (Length != BLE_BOND_ARRAY_LEN)
        return false;
    memcpy(BondsTriggerValue, Data, BLE_BOND_ARRAY_LEN);
    return true;
}

// The Window write is a Uint32 command (set the deadline, 0 closes).
bool OnBleWindowWrite(const StaticBlockDescriptor &, uint16_t, const void *Data, uint16_t Length)
{
    WindowTriggerCalls++;
    if (Length != sizeof(uint32_t))
        return false;
    memcpy(&WindowTriggerValue, Data, sizeof(WindowTriggerValue));
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

    // --- six fields, numbered 0..5, and nothing else ---
    CHECK(BleSettingsSchema.EntryCount == 6);
    CHECK(BleSettingsSchema.TriggerCount == 5); // every settable field but Mode
    CHECK(Blk.Get(6, 0).Data == nullptr);       // the retired Action / Advertised name / Last* slots
    CHECK(Blk.Get(11, 0).Data == nullptr);

    // --- the fields resolve to the struct members the offsets claim ---
    FieldResult R = Blk.Get(BLE_SET_FIELD_ENABLED, 0);
    CHECK(R.Data == &Per.Enabled);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Bool);
    CHECK(R.Descriptor.Size == 1);
    CHECK(ValueIsPersistent(R.Descriptor));
    CHECK(!ValueIsReadOnly(R.Descriptor));

    // The passkey is readable and writable, but NOT persistent: it lives in NVS, so the register
    // file (.SV) never carries the secret. Its wire copy is the volatile field.
    R = Blk.Get(BLE_SET_FIELD_PASSKEY, 0);
    CHECK(R.Data == &Vol.Passkey);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Text);
    CHECK(R.Descriptor.Size == BLE_PASSKEY_LEN);
    CHECK(!ValueIsPersistent(R.Descriptor));
    CHECK(!ValueIsReadOnly(R.Descriptor)); // readable back: the app may show the user their code

    R = Blk.Get(BLE_SET_FIELD_MODE, 0);
    CHECK(R.Data == &Vol.Mode);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Enum);
    CHECK(R.Descriptor.Size == 1);
    CHECK(ValueIsReadOnly(R.Descriptor));

    R = Blk.Get(BLE_SET_FIELD_DISPLAYNAME, 0);
    CHECK(R.Data == &Per.DisplayName);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Text);
    CHECK(R.Descriptor.Size == BLE_DISPLAY_NAME_LEN);
    CHECK(ValueIsPersistent(R.Descriptor));
    CHECK(!ValueIsReadOnly(R.Descriptor));

    // The bond list: ONE Undefined value (a raw byte block - the app decodes it), writable, and its
    // declared size is what fixes the entry count.
    R = Blk.Get(BLE_SET_FIELD_BONDS, 0);
    CHECK(R.Data == &Vol.Bonds);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Undefined);
    CHECK(R.Descriptor.Size == BLE_BOND_ARRAY_LEN);
    CHECK(!ValueIsReadOnly(R.Descriptor));
    CHECK(!ValueIsPersistent(R.Descriptor));

    // The entry geometry the size is built from: 6-byte address + 16-byte name, three slots.
    CHECK(BLE_BOND_ENTRY_LEN == 22);
    CHECK(BLE_SETTINGS_BOND_SLOTS == 3);
    CHECK(BLE_BOND_ARRAY_LEN == BLE_SETTINGS_BOND_SLOTS * BLE_BOND_ENTRY_LEN);
    CHECK(sizeof(Vol.Bonds) == BLE_BOND_ARRAY_LEN);
    // ... and an entry is address-then-name at the stride the field promises.
    CHECK((void *)(Vol.Bonds + 1 * BLE_BOND_ENTRY_LEN) == (void *)(&Vol.Bonds[BLE_BOND_ENTRY_LEN]));
    CHECK(BLE_BOND_NAME_LEN == BLE_DISPLAY_NAME_LEN);

    // Window: a Uint32 that now carries a command as well as the countdown.
    R = Blk.Get(BLE_SET_FIELD_WINDOW, 0);
    CHECK(R.Data == &Vol.WindowMs);
    CHECK(ValueInfoType(R.Descriptor) == (uint16_t)DataType::Uint32);
    CHECK(R.Descriptor.Size == sizeof(uint32_t));
    CHECK(!ValueIsReadOnly(R.Descriptor));

    // --- the empty/invalid address rule the array is read with ---
    const uint8_t Empty[BLE_BOND_ADDR_LEN] = {0, 0, 0, 0, 0, 0};
    const uint8_t Peer[BLE_BOND_ADDR_LEN] = {0x4C, 0x23, 0x38, 0x74, 0xE6, 0xF6};
    CHECK(BleBondAddrEmpty(Empty));
    CHECK(!BleBondAddrEmpty(Peer));
    const uint8_t NearlyEmpty[BLE_BOND_ADDR_LEN] = {0, 0, 0, 0, 0, 1};
    CHECK(!BleBondAddrEmpty(NearlyEmpty)); // one non-zero byte is an address, not an empty slot

    // --- the defaults a device with a zero .SV tail gets ---
    CHECK(Per.Enabled == 1);                        // radio on out of the box
    CHECK(memcmp(Per.ScrubbedPasskey, "000000", BLE_PASSKEY_LEN) == 0); // nothing inherited
    CHECK(memcmp(Vol.Passkey, "000000", BLE_PASSKEY_LEN) == 0);         // Just Works
    CHECK(Per.DisplayName[0] == '\0');              // empty = use System.Name
    CHECK(Vol.Mode == BLE_SET_MODE_JUST_WORKS);
    CHECK(Vol.WindowMs == 0);                       // closed
    for (uint8_t i = 0; i < BLE_BOND_ARRAY_LEN; i++)
        CHECK(Vol.Bonds[i] == 0);                   // no bond: every slot reads as the empty address

    // --- Read Only is refused, not silently ignored ---
    uint8_t One = 1;
    ValueInfo BoolDesc = {(uint16_t)DataType::Bool, 1, 0};
    CHECK(!Blk.Set(BLE_SET_FIELD_MODE, 0, &One, 1, BoolDesc));

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

    // --- the Bonds write reaches its trigger as the whole array ---
    uint8_t Array[BLE_BOND_ARRAY_LEN] = {0};
    memcpy(Array, Peer, BLE_BOND_ADDR_LEN);          // entry 0's address
    memcpy(Array + BLE_BOND_ADDR_LEN, "Tamu rig", 8); // ... and its name
    ValueInfo BondDesc = {(uint16_t)DataType::Undefined, BLE_BOND_ARRAY_LEN, 0};
    // A Text-typed write is refused above the trigger: the field is one raw block.
    ValueInfo WrongType = {(uint16_t)DataType::Text, BLE_BOND_ARRAY_LEN, 0};
    CHECK(!Blk.Set(BLE_SET_FIELD_BONDS, 0, Array, BLE_BOND_ARRAY_LEN, WrongType));
    CHECK(BondsTriggerCalls == 0);
    // A value shorter than the declared size is refused too - the dispatcher caps Length to the
    // descriptor, and Set() refuses a non-Text field whose length is not its declared size.
    CHECK(!Blk.Set(BLE_SET_FIELD_BONDS, 0, Array, BLE_BOND_ADDR_LEN, BondDesc));
    CHECK(BondsTriggerCalls == 0);
    CHECK(Blk.Set(BLE_SET_FIELD_BONDS, 0, Array, BLE_BOND_ARRAY_LEN, BondDesc));
    CHECK(BondsTriggerCalls == 1);
    CHECK(memcmp(BondsTriggerValue, Peer, BLE_BOND_ADDR_LEN) == 0);
    CHECK(memcmp(BondsTriggerValue + BLE_BOND_ADDR_LEN, "Tamu rig", 8) == 0);
    CHECK(memcmp(Vol.Bonds, Array, BLE_BOND_ARRAY_LEN) != 0); // the trigger applies it, not Set()

    // --- the Window write reaches its trigger as a Uint32 ---
    ValueInfo U32Desc = {(uint16_t)DataType::Uint32, sizeof(uint32_t), 0};
    uint32_t Ms = 45000;
    CHECK(!Blk.Set(BLE_SET_FIELD_WINDOW, 0, &Ms, 1, U32Desc)); // short value: never reaches the trigger
    CHECK(WindowTriggerCalls == 0);
    CHECK(Blk.Set(BLE_SET_FIELD_WINDOW, 0, &Ms, sizeof(Ms), U32Desc));
    CHECK(WindowTriggerCalls == 1);
    CHECK(WindowTriggerValue == Ms);
    Ms = 0; // 0 closes it - the same field, the same trigger
    CHECK(Blk.Set(BLE_SET_FIELD_WINDOW, 0, &Ms, sizeof(Ms), U32Desc));
    CHECK(WindowTriggerCalls == 2);
    CHECK(WindowTriggerValue == 0);
}

// --- the bond array's refresh decision ----------------------------------------
//
// The block's Bonds array is a cache of the NimBLE bond store. Refreshing it on the store's SIZE
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

// --- which passkey write invalidates the stored bonds --------------------------
//
// The decision the device makes on every policy apply (AppBLEEnforceBondPolicy in AppBLE.h). It is
// also the answer that drops a live session, so "a passkey change revokes the bonds" and "the link
// they authenticated is gone" are one rule. The two failure modes this table pins are the ones the
// rig found: a re-written passkey that revokes nothing is useless, and a passkey change to
// `000000` that leaves the old peer's session alive is worse.
static void TestBondRevocation()
{
    // A write of the SAME code is not a revocation: revoke nothing, drop nothing.
    CHECK(!BleBondsInvalidated(true, "123456", "123456"));
    CHECK(!BleBondsInvalidated(true, "000000", "000000"));

    // Any real change is one, in EITHER direction - `000000` included, because the old code is
    // gone and the peer that authenticated with it is a stranger now.
    CHECK(BleBondsInvalidated(true, "000000", "123456")); // a passkey is set
    CHECK(BleBondsInvalidated(true, "123456", "000000")); // the passkey is removed
    CHECK(BleBondsInvalidated(true, "123456", "654321")); // rotated
    CHECK(BleBondsInvalidated(true, "123456", "123457")); // one digit

    // An unstamped store (a fresh partition, or a build that predates the stamp) holds Just Works
    // bonds: adopting it under Just Works keeps them; a passkey revokes them once.
    CHECK(!BleBondsInvalidated(false, "000000", "000000"));
    CHECK(BleBondsInvalidated(false, "000000", "123456"));

    // A current value the store could never hold never equals its stamp either (the epoch read
    // from NVS is validated the same way, so a half-written blob cannot pass for a match).
    CHECK(BleBondsInvalidated(true, "123456", "12345"));
    CHECK(BleBondsInvalidated(true, "123456", "12a456"));

    // The table is symmetric in the digits: same length, same text, whatever the surrounding
    // buffer looks like (the comparison is bounded by BLE_PASSKEY_LEN).
    char Epoch[16] = {'1', '2', '3', '4', '5', '6', 'x', 'x'};
    char Now[16] = {'1', '2', '3', '4', '5', '6', 'y', 'y'};
    CHECK(!BleBondsInvalidated(true, Epoch, Now));
}

int main()
{
    TestPolicy();
    TestBlockTable();
    TestBondRefresh();
    TestSecureBudget();
    TestBondRevocation();

    printf("ble_settings: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
