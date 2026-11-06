#pragma once

// The device half of the naming rule (Blocks/AutoName.h): what the FIRST bond writes.
//
// Included from Main.h AFTER AppBLE.h - the flag the host callback raises lives there - and BEFORE
// BleSettingsGlue.h, whose per-loop hook is its only caller. The register file is not touched from
// the NimBLE host callback: the write is a System/Name write plus ONE `.SV` mirror write, and a
// flash write does not belong in a connection callback.
//
// WHAT IT COSTS: on the first bond only, one 16-byte field write and one `.SV` mirror write (the
// same single write a Save All performs). Nothing at all on a reconnect, nothing on a later pairing
// (the field is no longer undisturbed), and nothing when the owner - or a setup restore - has
// already named the device.
//
// WHERE THE NAME LIVES: System `Name` (field 6) in the register file, mirrored 1:1 to `.SV` with
// the rest of the static persistent space, so a boot's recall restores it (RegisterPersist.h
// StaticRecallAll). The BLE advertisement carries it while the `BLE Settings` block's Display name
// is empty, and - like any other write of System/Name - it reaches the air at the next radio start
// ("Applies fully after reboot", Docs/Services/System Block and Device Commands.md field 6), which
// is the restart the rig pass measures.

#include "Blocks/AutoName.h"
#include "Core/Services/StaticMemory.h"   // SYSTEM_NAME_LEN, SystemPersistent, StaticValuesName
#include "Core/Functions/MemoryBackup.h"  // WriteBackupFile: the one `.SV` mirror write

// The identity bytes GetSerialNumber() fills - the low 48 bits of the factory MAC (Main.h above),
// which is the part of the serial that is unique to this device.
constexpr uint8_t KSerialIdentityLen = 6;

// Writes the auto-name for the first bond this device made, if it may. Called once per loop pass
// from BleSettingsSync; does nothing while no first bond is owed.
//
// The one-shot flag is consumed even when the write is refused: "one attempt per first bond" is the
// rule, and a field somebody has named must not be examined on every pass for the rest of the run.
bool AppBLEAutoNameService()
{
    if (!BleAutoNameOwed)
        return false;
    BleAutoNameOwed = false; // one attempt per first bond (a re-pair on the same link owes nothing)

    const bool Undisturbed = AutoNameUndisturbed(StaticPer.System.Name, SYSTEM_NAME_LEN,
                                                 KFactorySystem.Name, SYSTEM_NAME_LEN);
    // The snapshot is the one taken when this link came up (AppBLELinkBondSnapshot, in onConnect),
    // so `BleLinkBondCount == 0` IS "this link made the device's first bond" - the same fact the
    // session gate admitted the link on (BleSessionAllowed's empty-store case).
    if (!AutoNameDue(BleLinkBondCount == 0, AppBLEBondCount(), Undisturbed))
    {
        DeviceLog("BLESEC", "first bond: System name left as it is (%s)",
                  Undisturbed ? "no bond to name" : "already named");
        return false;
    }

    char Wanted[AUTO_NAME_MAX_LEN + 1];
    const uint8_t Len = AutoNameFormat(Wanted, sizeof(Wanted), AutoNamePrefix(KDeviceType),
                                       GetSerialNumber().Bytes, KSerialIdentityLen);
    if (Len == 0)
    {
        // Every device would carry the same name: leave the field alone rather than do that.
        DeviceLog("BLESEC", "first bond: no name could be built from the serial");
        return false;
    }

    AutoNameFieldSet(StaticPer.System.Name, SYSTEM_NAME_LEN, Wanted, Len);

    // The mirror write StaticSaveAll performs (the service's own path is private): the name has to
    // survive a restart, and `.SV` is what a boot recalls. Snapshot the live net-ID on the way, as
    // that path does - the stored value is the one that applies on reboot.
#ifdef TYPE_CORE
    StaticPer.System.NetId = DeviceStatus.NetId;
#endif
    const bool Saved = WriteBackupFile(StaticValuesName(), (const uint8_t *)&StaticPer, sizeof(StaticPer));
    if (!Saved)
    {
        // The name is in force in RAM; it is just not durable. Say so rather than claim a name the
        // next boot would revert.
        DeviceLog("BLESEC", "first bond: named '%s' but the .SV write failed (RAM only)", Wanted);
        return false;
    }
    DeviceLog("BLESEC", "first bond: named '%s' and stored it", Wanted);
    return true;
}
