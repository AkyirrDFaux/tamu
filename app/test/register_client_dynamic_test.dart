import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';

/// Pins the dynamic-memory command request shape: the create/delete/set-name commands carry
/// one global `Index (uint16)` little-endian (Docs/Services/Register.md "Dynamic commands"),
/// not the old single-byte block number.
class _CapturingRegisterClient extends RegisterClient {
  _CapturingRegisterClient() : super(deviceId: 1);

  final calls = <({int cid, List<int> payload})>[];
  final replies = <int, List<int>>{};

  @override
  Future<List<int>?> request(int cid,
      {List<int> payload = const [], Duration? timeout}) async {
    calls.add((cid: cid, payload: List<int>.from(payload)));
    return replies[cid];
  }
}

void main() {
  test('create carries Index (uint16) little-endian + the padded 16-char name', () async {
    final c = _CapturingRegisterClient();
    c.replies[DynamicCid.create] = [0x34, 0, 0, 0, 0]; // BlockIndex echo + padding

    final assigned = await c.createDynamicBlock('Box', index: 0x1234);
    expect(assigned, 0x34);

    final call = c.calls.single;
    expect(call.cid, DynamicCid.create);
    expect(call.payload.length, 18); // Index(2) + Name(16)
    expect(call.payload.sublist(0, 2), [0x34, 0x12]); // 0x1234 LE
    expect(call.payload.sublist(2, 5), 'Box'.codeUnits);
    // Name is space-padded to the fixed 16-char field.
    expect(call.payload.sublist(5), List<int>.filled(13, 0x20));
  });

  test('create with a small index still writes the full uint16', () async {
    final c = _CapturingRegisterClient();
    c.replies[DynamicCid.create] = [0xAB, 0, 0, 0, 0];
    await c.createDynamicBlock('N', index: 0x00AB);
    expect(c.calls.single.payload.sublist(0, 2), [0xAB, 0x00]);
  });

  test('delete carries Index (uint16) little-endian', () async {
    final c = _CapturingRegisterClient();
    c.replies[DynamicCid.delete] = [0]; // status 0 = success

    expect(await c.deleteDynamic(block: 0x1234), isTrue);
    final call = c.calls.single;
    expect(call.cid, DynamicCid.delete);
    expect(call.payload, [0x34, 0x12]);
  });

  test('set-name carries Index (uint16) little-endian + the padded name', () async {
    final c = _CapturingRegisterClient();
    c.replies[DynamicCid.setName] = [0];
    final block = DynBlock(index: 0x1234, meta: ValueInfo(type: dynamicTypeBase), name: '');

    expect(await c.writeDynamicBlockMeta(block, 'Renamed', null), isTrue);
    final call = c.calls.single;
    expect(call.cid, DynamicCid.setName);
    expect(call.payload.sublist(0, 2), [0x34, 0x12]);
    expect(call.payload.sublist(2, 9), 'Renamed'.codeUnits);
  });
}
