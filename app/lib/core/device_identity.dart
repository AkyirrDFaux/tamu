/// One device's identity, read from its System block
/// (Docs/Services/System Block and Device Commands.md).
///
/// Pure Dart, and the single reader of those fields: the app's in-RAM device database and a
/// restore's live snapshot both come through here, so the two cannot disagree about a device's
/// name, serial, type or capabilities.
library;

import 'protocol.dart';
import 'register_client.dart';
import 'service_request.dart';
import 'types.dart';

/// Everything the System block says about a device.
class DeviceIdentity {
  final int id;

  /// The reported System Name, with a per-id fallback when the device reports none.
  final String name;
  final String? serial; // 28-char hex
  final String? softwareVersion;
  final int capabilities;
  final int typeValue;

  DeviceIdentity({
    required this.id,
    required this.name,
    this.serial,
    this.softwareVersion,
    required this.capabilities,
    required this.typeValue,
  });

  DeviceType get type => DeviceType.fromValue(typeValue);
}

/// Reads [id]'s identity: a ping first, then the System fields. Returns null when the device
/// does not answer the ping - it is not reachable on the bus.
///
/// [readAttempts] is passed through to the Register reads: a caller that decides device SHAPE
/// from this identity (a restore's snapshot, the setup tool's rig scan) passes
/// `RegisterClient.snapshotReadAttempts`, because a lost Device Type read reports type 0 - a
/// device of the wrong type - rather than a read that never arrived.
Future<DeviceIdentity?> readDeviceIdentity(
  int id, {
  int readAttempts = 1,
}) async {
  final ping = await serviceRequest(
    id,
    ServiceType.device,
    1,
    timeout: const Duration(milliseconds: 800),
    tag: 'identity',
  );
  if (ping == null) return null;

  final reg = RegisterClient(deviceId: id, readAttempts: readAttempts);
  // System fields are exposed as one entry per field at key 0 (the struct position is not
  // on the wire; Docs/Services/System Block and Device Commands.md).
  String? serial;
  final snReply = await reg.readField(1, 0);
  if (snReply != null && snReply.value.length >= 14) {
    serial = serialNumberToHex(snReply.value.sublist(0, 14));
  }
  var typeValue = 0;
  final typeReply = await reg.readField(0, 0);
  if (typeReply != null && typeReply.value.length >= 2) {
    typeValue = typeReply.value[0] | (typeReply.value[1] << 8);
  }
  String? version;
  final versionReply = await reg.readField(0, 2);
  if (versionReply != null && versionReply.value.length >= 4) {
    version = formatSoftwareVersion(versionReply.value);
  }
  var capabilities = 0;
  final capReply = await reg.readField(0, 1);
  if (capReply != null && capReply.value.length >= 4) {
    capabilities = uint32FromBytes(capReply.value);
  }
  var name = 'Device ${idToString(id)}';
  final nameReply = await reg.readField(6, 0);
  if (nameReply != null && nameReply.value.isNotEmpty) {
    name = decodePaddedString(nameReply.value);
  }
  if (name.isEmpty) name = 'Device ${idToString(id)}';

  return DeviceIdentity(
    id: id,
    name: name,
    serial: serial,
    softwareVersion: version,
    capabilities: capabilities,
    typeValue: typeValue,
  );
}
