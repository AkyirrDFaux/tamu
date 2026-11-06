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
// 0 (unassigned) and 0x3F (all nets) are not valid nets: a write of either is refused, and the
// boot's "restored, or a random one" rule (Main.h) fills in a usable value.

#include "Blocks/DeviceInfo.h"   // DeviceStatusStruct, which Packet.h refers to (the device TUs get it
                                // in this order already; spelled out here so this header stands alone)
#include "Core/Functions/Packet.h"

inline bool NetIdStorable(uint8_t NetId) { return NetId != 0 && NetId < 0x3F; }

// Applies a written NetID to the live status: the net, and the address every packet's id_src is
// taken from. Returns false - and changes nothing - for a value that is not a net.
inline bool ApplyNetId(uint8_t &LiveNetId, uint16_t &ShortAddress, uint8_t Stored) {
    if (!NetIdStorable(Stored)) return false;
    LiveNetId = Stored;
    ShortAddress = MakeId(Stored, 1); // a core is always NetID.1
    return true;
}
