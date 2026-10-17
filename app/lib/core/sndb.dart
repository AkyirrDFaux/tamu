/// The core's Serial Number Database (SNDB): the network roster, and the writes that maintain
/// it (Docs/Services/System Block and Device Commands.md, 00.12/00.13).
///
/// Pure Dart, and the single place the SNDB record format is decoded: the app's in-RAM device
/// database and a restore's SNDB items both come through here.
library;

import 'protocol.dart';
import 'service_request.dart';
import 'types.dart';

/// The SNDB roster as `[id, serial hex]` records, including id 0 (the unassigned marker -
/// callers filter it). Empty when the core does not answer.
Future<List<({int id, String serial})>> sndbEntries() async {
  final reply = await serviceRequest(
    coreId,
    ServiceType.device,
    0x13,
    timeout: const Duration(seconds: 5),
    tag: 'sndb',
  );
  if (reply == null) return const [];
  return parseSndb(reply);
}

/// The single SNDB record parser: the 16-byte entries are `serial (14) | id (u16)`
/// little-endian. Every record is emitted (including id 0, the unassigned marker); callers
/// filter as needed.
List<({int id, String serial})> parseSndb(List<int> reply) => [
  for (var offset = 0; offset + 16 <= reply.length; offset += 16)
    (
      id: reply[offset + 14] | (reply[offset + 15] << 8),
      serial: serialNumberToHex(reply.sublist(offset, offset + 14)),
    ),
];

/// SNDB write (00.12): registers [serial] under [id]. The core answers with the updated
/// record, so a reply of a full record is the acceptance signal.
Future<bool> sndbWrite(List<int> serial, int id) async {
  final reply = await serviceRequest(
    coreId,
    ServiceType.device,
    0x12,
    payload: [...serial, id & 0xFF, (id >> 8) & 0xFF],
    timeout: const Duration(seconds: 3),
    tag: 'sndb',
  );
  return reply != null && reply.length >= 16;
}

/// SNDB delete (00.12 with ID 0): drops [serial]'s record.
Future<bool> sndbDelete(List<int> serial) async {
  if (serial.length < 14) return false;
  final reply = await serviceRequest(
    coreId,
    ServiceType.device,
    0x12,
    payload: [...serial.take(14), 0, 0],
    timeout: const Duration(seconds: 3),
    tag: 'sndb',
  );
  return reply != null && reply.length >= 16;
}
