#pragma once

// The BLE link security policy, derived from ONE place: the `BLE Settings` block's Passkey
// field (field 0, Text(6); "000000" = disabled = Just Works). Both the radio's pairing
// configuration and the GATT attribute permissions are computed here from that text, so the two
// can never disagree and the BLE link keeps no second copy of the policy. The text itself is
// stored in NVS rather than in the register file (Devices/Tamu_v2.0A/BlePasskeyStore.h), so the
// policy's source is a register field that is readable and writable but never persisted to `.SV`.
//
// Host-clean on purpose (no NimBLE, no ESP-IDF): test/native/ble_security_test.cpp drives the
// same decisions the device makes, which is the only part of this feature that can be verified
// without a phone on the air.
//
// The passkey always reaches the device over USB (the block's write rule refuses it on a BLE
// session) - it is never typed on the device, which has no display. `Mode` is derived from this
// text, never stored, so "which policy is in force" has exactly one answer.

#include <cstdint>

// ===== GATT characteristic flags (BLE_GATT_CHR_F_*) =====
// NimBLE expresses an attribute's permissions as its characteristic flags and enforces them in
// the ATT server, before any application code runs: a peer without an encrypted link (and, for
// the AUTHEN bits, without an authenticated one) is refused the access there. The values are
// restated here instead of including NimBLE so this header stays testable on the host.
enum BleAttFlag : uint16_t
{
    BleAttWrite              = 0x0008, // BLE_GATT_CHR_F_WRITE
    BleAttWriteNoResponse    = 0x0004, // BLE_GATT_CHR_F_WRITE_NO_RSP
    BleAttNotify             = 0x0010, // BLE_GATT_CHR_F_NOTIFY
    BleAttWriteEncrypted     = 0x1000, // BLE_GATT_CHR_F_WRITE_ENC
    BleAttWriteAuthenticated = 0x2000, // BLE_GATT_CHR_F_WRITE_AUTHEN
    BleAttNotifyEncrypted    = 0x8000, // BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC
    // BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN is 0x10000 and has no 16-bit form: NimBLE-cpp
    // passes a characteristic's flags as uint16_t (NimBLECharacteristic's `properties`), so the
    // notify path cannot carry it. It is not needed either - a passkey-protected pairing makes
    // encryption and authentication the same event, so NOTIFY_INDICATE_ENC already excludes an
    // unauthenticated peer from subscribing. The write path (the one that carries app commands)
    // does carry WRITE_AUTHEN.
};

// The 6 digits of the passkey field.
#define BLE_PASSKEY_LEN 6

// BLE_HS_IO_* input/output capabilities, restated for the same reason as the flags.
constexpr uint8_t BleIoCapDisplayOnly  = 0x00; // device shows the code, phone types it
constexpr uint8_t BleIoCapNoInputOutput = 0x03; // Just Works: no code anywhere

struct BleSecurityPolicy
{
    uint32_t Passkey;    // 0 = Just Works
    bool     Mitm;       // authentication required during pairing
    uint8_t  IoCap;      // BleIoCapDisplayOnly / BleIoCapNoInputOutput
    uint16_t RxFlags;    // app -> device characteristic (writable)
    uint16_t TxFlags;    // device -> app characteristic (notifying)
};

// True when `Txt` is exactly BLE_PASSKEY_LEN ASCII digits.
inline bool BlePasskeyDigits(const char *Txt)
{
    for (uint8_t i = 0; i < BLE_PASSKEY_LEN; i++)
    {
        if (Txt[i] < '0' || Txt[i] > '9')
            return false;
    }
    return true;
}

// True when `Txt` is a passkey that enables MITM-protected pairing: 6 digits, not `000000`.
// An all-zero code is the "disabled" sentinel (and 0 is not a legal SM passkey either).
inline bool BlePasskeyEnabled(const char *Txt)
{
    if (!BlePasskeyDigits(Txt))
        return false;
    for (uint8_t i = 0; i < BLE_PASSKEY_LEN; i++)
    {
        if (Txt[i] != '0')
            return true;
    }
    return false;
}

// The numeric passkey for BLE_SM / setSecurityPasskey. Meaningless unless BlePasskeyEnabled.
inline uint32_t BlePasskeyValue(const char *Txt)
{
    uint32_t Value = 0;
    for (uint8_t i = 0; i < BLE_PASSKEY_LEN; i++)
        Value = Value * 10 + (uint32_t)(Txt[i] - '0');
    return Value;
}

// The whole policy for a passkey field's text.
//
// Every characteristic permission requires encryption, and the AUTHEN bits are added when a
// passkey is set, so an unencrypted - or, with a passkey, an unauthenticated - peer is refused
// at the attribute instead of in application code.
inline BleSecurityPolicy BleSecurityPolicyFrom(const char *PasskeyText)
{
    BleSecurityPolicy Policy;
    Policy.Mitm = BlePasskeyEnabled(PasskeyText);
    Policy.Passkey = Policy.Mitm ? BlePasskeyValue(PasskeyText) : 0;
    Policy.IoCap = Policy.Mitm ? BleIoCapDisplayOnly : BleIoCapNoInputOutput;

    Policy.RxFlags = (uint16_t)(BleAttWrite | BleAttWriteNoResponse | BleAttWriteEncrypted);
    if (Policy.Mitm)
        Policy.RxFlags |= BleAttWriteAuthenticated;

    // The notify characteristic has no value read or write: the only thing a peer can do with
    // it is write the CCCD to subscribe, and that write is what NOTIFY_INDICATE_ENC protects
    // (the NimBLE ATT server derives the CCCD's write permissions from this flag).
    Policy.TxFlags = (uint16_t)(BleAttNotify | BleAttNotifyEncrypted);
    return Policy;
}

// ===== Does a passkey write invalidate the stored bonds? =====
//
// The bond store is stamped with the passkey TEXT it was built under
// (Devices/Tamu_v2.0A/BlePasskeyStore.h), and this is the comparison the device makes on every
// policy apply and again at radio start (AppBLEEnforceBondPolicy in AppBLE.h). It is also the
// answer that decides whether a LIVE session is dropped, so "revokes the bonds" and "drops the
// link they authenticated" can never disagree.
//
// `EpochKnown == false` is the "no stamp yet" third state: a fresh partition, or a device upgraded
// from a build that predates the stamp. The only build that could have written such a store had no
// passkey at all, so its bonds are Just Works bonds - kept while the device is still on Just Works
// (no re-pairing for every upgrade) and revoked once under a passkey, which is the safe direction.
//
// Every other difference is a change of credential, in EITHER direction: the bonds and any live
// session were authenticated with a code that no longer exists, so `000000` (Just Works) is a
// change like any other - a one-way "is MITM now required" test would miss it. A write of the SAME
// text is NOT a change: it must revoke nothing and drop nothing.
inline bool BleBondsInvalidated(bool EpochKnown, const char *Epoch, const char *Now)
{
    if (!BlePasskeyDigits(Now))
        return true; // a value the store can never hold is never the stamp it holds
    if (!EpochKnown)
        return BlePasskeyEnabled(Now);
    for (uint8_t i = 0; i < BLE_PASSKEY_LEN; i++)
    {
        if (Epoch[i] != Now[i])
            return true;
    }
    return false;
}

// ===== How long a new link may stay unsecured =====
//
// The device drops a link that has not completed pairing by its deadline (AppBLETick in
// Devices/Tamu_v2.0A/AppBLE.h). The budget is policy-shaped because the phone's half of the
// procedure is: Just Works is one tap - or nothing at all on iOS - and is over in well under a
// second, while a passkey makes the phone show a dialog and a PERSON type six digits. One short
// deadline for both drops every passkey pairing half-finished, which from the phone's side looks
// exactly like "the code was wrong".
//
// The clock is granted at connect and not restarted when pairing starts, because there is no
// application-visible "pairing started" event to restart it from: nimble-cpp injects the
// configured passkey straight out of the BLE_GAP_EVENT_PASSKEY_ACTION handler
// (NimBLEServer.cpp:669-683, ble_sm_inject_io) and only calls onPassKeyDisplay() when the static
// passkey is still its 123456 default. The procedure begins as soon as onConnect asks for
// security, so "from connect" is "from the prompt" for all practical purposes - the passkey
// figure is simply sized for a human, and both figures stay inside one connectable window.
constexpr uint32_t BleSecureBudgetJustWorksMs = 5000;
constexpr uint32_t BleSecureBudgetPasskeyMs = 30000;

inline uint32_t BleSecureBudgetMs(const BleSecurityPolicy &Policy)
{
    return Policy.Mitm ? BleSecureBudgetPasskeyMs : BleSecureBudgetJustWorksMs;
}
