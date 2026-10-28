#pragma once

// The BLE link security policy, derived from ONE place: the `BLE Security` block's Passkey
// field (field 0, Text(6), Persistent; "000000" = disabled = Just Works). Both the radio's
// pairing configuration and the GATT attribute permissions are computed here from that text,
// so the two can never disagree and the BLE link keeps no second copy of the policy.
//
// Host-clean on purpose (no NimBLE, no ESP-IDF): test/native/ble_security_test.cpp drives the
// same decisions the device makes, which is the only part of this feature that can be verified
// without a phone on the air.
//
// The passkey always reaches the device over USB (the block's write rule refuses it on a BLE
// session) - it is never typed on the device, which has no display.

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
