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
/// The five System reads go out TOGETHER rather than one after another. Each is an independent
/// field of the same block, and the wire round trip (measured on the rig over BLE: ~22 ms per
/// request) is most of what each one costs, so reading them concurrently is what makes an app
/// start quick instead of spending five round trips per device to learn a name.
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
  final snReply = reg.readField(1, 0);
  final typeReply = reg.readField(0, 0);
  final versionReply = reg.readField(0, 2);
  final capReply = reg.readField(0, 1);
  final nameReply = reg.readField(6, 0);
  final replies = await Future.wait([
    snReply,
    typeReply,
    versionReply,
    capReply,
    nameReply,
  ]);
  final sn = replies[0];
  final type = replies[1];
  final version = replies[2];
  final caps = replies[3];
  final nameField = replies[4];

  String? serial;
  if (sn != null && sn.value.length >= 14) {
    serial = serialNumberToHex(sn.value.sublist(0, 14));
  }
  var typeValue = 0;
  if (type != null && type.value.length >= 2) {
    typeValue = type.value[0] | (type.value[1] << 8);
  }
  String? softwareVersion;
  if (version != null && version.value.length >= 4) {
    softwareVersion = formatSoftwareVersion(version.value);
  }
  var capabilities = 0;
  if (caps != null && caps.value.length >= 4) {
    capabilities = uint32FromBytes(caps.value);
  }
  var name = 'Device ${idToString(id)}';
  if (nameField != null && nameField.value.isNotEmpty) {
    name = decodePaddedString(nameField.value);
  }
  if (name.isEmpty) name = 'Device ${idToString(id)}';

  return DeviceIdentity(
    id: id,
    name: name,
    serial: serial,
    softwareVersion: softwareVersion,
    capabilities: capabilities,
    typeValue: typeValue,
  );
}
