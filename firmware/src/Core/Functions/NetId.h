#pragma once

// The core's net ID (Docs/Services/System Block and Device Commands.md, System field 7).
//
// ONE rule, shared by the register write that sets it and by the save paths that mirror the
// static space into `.SV`: the live net and the stored net are the same value, and a core's bus
// address is always NetID.1.
//
// Why the write applies the net live instead of only storing it for the next boot:
//   - `StaticSaveAll` (and the BLE name/passkey mirrors) snapshot `DeviceStatus.NetId` into the
//     static space before writing `.SV` (RegisterPersist.h, BleAutoName.h, BleSettingsGlue.h).
//     A NetID written into the space ALONE is therefore reverted by the next Save All - and a
//     setup restore writes the System block and then saves, so a restored NetID never survived:
//     the core came up on the net it already had (or on the random one a wiped space gets) while
//     the nodes and the archive were on the setup's net.
//   - With the two halves disagreeing, the core addresses its own bus traffic with the wrong
//     net. A subscription install (`CID 0x01 Change`) is what found this: the row's provider
//     address is re-qualified to the core's net at every use (RequesterProviderAddr,
//     SubscriptionsRequester.h), so every Change went to a device that does not exist and the
//     nodes' provider tables stayed empty (measured on the rig, t_ab3e764a: the core's own log
//     says `prov 3803` - net 14 - while the node answers at 39.3).
//   - A node already works this way: it adopts the net of an assignment live (Device.h,
//     `DeviceStatus.NetId = ...`). The core had no equivalent, so the change could never be made
//     without a reset - which is exactly the state a wiped-core restore leaves behind.
//
// Why the move lands *after* the write's own reply (t_cac38f19):
//   The acknowledgement of a write is built from the live identity - `ShortAddress` stamps its
//   src (Packet.h, `IdSrc`) and `NetId` is what resolves the request's net-0 target back to us
//   (Dispatcher.h, `NetQualifyLocal`) - so a write that moves the identity first cannot answer
//   itself. Both halves have to move together (`NetId` alone would already change how the reply's
//   target is resolved), and only once the reply has been dispatched. Measured on the rig
//   (`firmware/.pio/probe/netid_set.py 39`): before the fix the write applied (live net 51 -> 39)
//   but its reply came back from 39.1 while the request had been addressed at 51.1, the
//   dispatcher treated it as a foreign frame instead of an app reply, and the client reported
//   `no matched reply` - the app logged the same write as "the write did not land" while it had.
//   Every client keys on the address it used, so the fix belongs here and not in a client.
//
// 0 (unassigned) and 0x3F (all nets) are not valid nets: a write of either is refused, and the
// boot's "restored, or a random one" rule (Main.h) fills in a usable value.

#include "Blocks/DeviceInfo.h"   // DeviceStatusStruct, which Packet.h refers to (the device TUs get it
                                // in this order already; spelled out here so this header stands alone)
#include "Core/Functions/Packet.h"

inline bool NetIdStorable(uint8_t NetId) { return NetId != 0 && NetId < 0x3F; }

// The net a write has accepted but not yet taken live; 0xFF (not a net) = nothing armed.
// A function-local so the header stays a plain inline definition in every translation unit.
inline uint8_t &ArmedNetIdSlot() {
    static uint8_t Armed = 0xFF;
    return Armed;
}

// Validates a written net and arms the move. Returns false - and changes nothing - for a value
// that is not a net. The write's own reply still goes out on the old identity.
inline bool ArmNetId(uint8_t Stored) {
    if (!NetIdStorable(Stored)) return false;
    ArmedNetIdSlot() = Stored;
    return true;
}

// Takes an armed net live: the net, and the address every packet's id_src is taken from. The
// write path calls this immediately after that write's reply has been dispatched, so the two
// halves move together and never in the middle of the transaction that caused the change.
inline void ApplyArmedNetId(uint8_t &LiveNetId, uint16_t &ShortAddress) {
    const uint8_t Stored = ArmedNetIdSlot();
    if (!NetIdStorable(Stored)) { ArmedNetIdSlot() = 0xFF; return; } // nothing armed (0xFF)
    ArmedNetIdSlot() = 0xFF;
    LiveNetId = Stored;
    ShortAddress = MakeId(Stored, 1); // a core is always NetID.1
}
