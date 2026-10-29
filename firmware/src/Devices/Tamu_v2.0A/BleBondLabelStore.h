#pragma once

// Where a bond's peer name is kept: NVS, beside the passkey.
//
// The NimBLE bond store keeps no peer name (Issues.md E60): a bond is the peer's identity address
// and its keys, nothing more. The `BLE Settings` block publishes each bond as "address + name"
// (Blocks/BleSettings.h), so the name half is this store's: a short label the host writes into an
// entry. It is keyed by the ADDRESS and never by slot, because the store reorders itself - a fourth
// phone evicts the oldest bond, and deleting one shifts the rest down - so a slot-keyed label would
// silently attach itself to a different phone.
//
// Nothing here is a secret or a credential: the label is only ever read back into the block's
// `Bonds` array. It lives in NVS (namespace `ble`, key `bondnames`) rather than in `.SV` because
// `.SV` is a file the Storage service streams out by name, and because the label is meaningless
// without the bond - it is bookkeeping for the store, and it shares the store's lifetime.

#include <cstring>
#include "Core/Functions/Log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "Blocks/BleSettings.h"          // BLE_SETTINGS_BOND_SLOTS, BLE_BOND_ADDR_LEN, BLE_BOND_NAME_LEN
#include "BlePasskeyStore.h"             // BlePasskeyStoreReady, BLE_PASSKEY_NVS_NS

#define BLE_BOND_LABEL_NVS_KEY "bondnames"

// One label. `Addr` is the peer's identity address in the order the block publishes it; all-zero
// means the entry is unused (a real identity address is never all zero).
struct BleBondLabel
{
    uint8_t Addr[BLE_BOND_ADDR_LEN];
    char    Name[BLE_BOND_NAME_LEN];
};

// The table is exactly as wide as the bond store, and is kept pruned to the bonds that exist: the
// glue hands over the live addresses on every store change (BleBondLabelPrune), so a slot is only
// ever occupied by a bond that is really there.
static BleBondLabel BleBondLabelTable[BLE_SETTINGS_BOND_SLOTS];
static bool BleBondLabelLoaded = false;

// Reads the whole table from NVS once. A missing or short blob leaves every entry empty, which reads
// as "no names" - the bonds themselves are unaffected.
static void BleBondLabelLoad()
{
    if (BleBondLabelLoaded)
        return;
    BleBondLabelLoaded = true;
    if (!BlePasskeyStoreReady())
        return;

    nvs_handle_t H;
    if (nvs_open(BLE_PASSKEY_NVS_NS, NVS_READONLY, &H) != ESP_OK)
        return; // the namespace does not exist yet: nothing has ever been labelled

    BleBondLabel Buf[BLE_SETTINGS_BOND_SLOTS];
    size_t Len = sizeof(Buf);
    const esp_err_t Rc = nvs_get_blob(H, BLE_BOND_LABEL_NVS_KEY, Buf, &Len);
    nvs_close(H);

    if (Rc == ESP_OK && Len == sizeof(Buf))
        memcpy(BleBondLabelTable, Buf, sizeof(Buf));
    else if (Rc != ESP_ERR_NVS_NOT_FOUND)
        DeviceLog("BLESEC", "stored bond names ignored (rc=%d len=%u)", (int)Rc, (unsigned)Len);
}

// Writes the whole table back. Returns false when the partition refused it, so a caller can refuse
// the register write instead of reading back a name that is gone after the next boot.
static bool BleBondLabelSave()
{
    if (!BlePasskeyStoreReady())
        return false;

    nvs_handle_t H;
    if (nvs_open(BLE_PASSKEY_NVS_NS, NVS_READWRITE, &H) != ESP_OK)
        return false;

    esp_err_t Rc = nvs_set_blob(H, BLE_BOND_LABEL_NVS_KEY, BleBondLabelTable, sizeof(BleBondLabelTable));
    if (Rc == ESP_OK)
        Rc = nvs_commit(H);
    nvs_close(H);

    if (Rc != ESP_OK)
        DeviceLog("BLESEC", "bond name store write failed (rc=%d)", (int)Rc);
    return Rc == ESP_OK;
}

// Copies the label of `Addr` into `Out` clamped to `Len` bytes, NUL-terminated. An address with no
// label reads as the empty string.
static void BleBondLabelGet(const uint8_t Addr[BLE_BOND_ADDR_LEN], char *Out, size_t Len)
{
    BleBondLabelLoad();
    if (!Out || Len == 0)
        return;
    Out[0] = '\0';
    for (int i = 0; i < BLE_SETTINGS_BOND_SLOTS; i++)
    {
        if (BleBondAddrEmpty(BleBondLabelTable[i].Addr) ||
            memcmp(BleBondLabelTable[i].Addr, Addr, BLE_BOND_ADDR_LEN) != 0)
            continue;
        size_t N = 0;
        while (N < BLE_BOND_NAME_LEN && BleBondLabelTable[i].Name[N] != '\0')
            N++;
        if (N >= Len)
            N = Len - 1;
        memcpy(Out, BleBondLabelTable[i].Name, N);
        Out[N] = '\0';
        return;
    }
}

// Stores `Name` (up to BLE_BOND_NAME_LEN bytes, trailing NULs and spaces trimmed) for `Addr`.
// Updates the slot that already holds this address, else takes a free one - there is always one for
// a live bond, because the table is pruned to the store (BleBondLabelPrune) and both are as wide as
// the store. Returns false when NVS refused the write, with the table left as it was.
static bool BleBondLabelSet(const uint8_t Addr[BLE_BOND_ADDR_LEN], const char *Name, size_t NameLen)
{
    BleBondLabelLoad();

    size_t N = NameLen > BLE_BOND_NAME_LEN ? BLE_BOND_NAME_LEN : NameLen;
    while (N > 0 && (Name[N - 1] == '\0' || Name[N - 1] == ' '))
        N--;

    BleBondLabel *Slot = nullptr;
    for (int i = 0; i < BLE_SETTINGS_BOND_SLOTS; i++)
        if (!BleBondAddrEmpty(BleBondLabelTable[i].Addr) &&
            memcmp(BleBondLabelTable[i].Addr, Addr, BLE_BOND_ADDR_LEN) == 0)
        {
            Slot = &BleBondLabelTable[i];
            break;
        }
    if (!Slot)
        for (int i = 0; i < BLE_SETTINGS_BOND_SLOTS; i++)
            if (BleBondAddrEmpty(BleBondLabelTable[i].Addr))
            {
                Slot = &BleBondLabelTable[i];
                break;
            }
    if (!Slot)
        return false; // every slot holds another bond's label: the caller's entry is not a live bond

    BleBondLabel Previous = *Slot;
    memset(Slot->Addr, 0x00, BLE_BOND_ADDR_LEN);
    memcpy(Slot->Addr, Addr, BLE_BOND_ADDR_LEN);
    memset(Slot->Name, 0x00, BLE_BOND_NAME_LEN);
    memcpy(Slot->Name, Name, N);
    if (BleBondLabelSave())
        return true;

    *Slot = Previous; // the store refused: leave no name that will not survive the next boot
    return false;
}

// Drops every label whose address is not one of the `LiveCount` addresses at `Live` (a flat array of
// BLE_BOND_ADDR_LEN-byte addresses, the bonds the store holds now). Keeps the table exactly as wide
// as the bonds, so a label can never be handed to a phone that inherited its slot. Saves only when
// something actually changed.
static bool BleBondLabelPrune(const uint8_t *Live, int LiveCount)
{
    BleBondLabelLoad();

    bool Changed = false;
    for (int i = 0; i < BLE_SETTINGS_BOND_SLOTS; i++)
    {
        if (BleBondAddrEmpty(BleBondLabelTable[i].Addr))
            continue;
        bool Alive = false;
        for (int k = 0; k < LiveCount; k++)
            if (memcmp(BleBondLabelTable[i].Addr, Live + k * BLE_BOND_ADDR_LEN, BLE_BOND_ADDR_LEN) == 0)
            {
                Alive = true;
                break;
            }
        if (!Alive)
        {
            BleBondLabelTable[i] = BleBondLabel{};
            Changed = true;
        }
    }
    return Changed ? BleBondLabelSave() : true;
}
