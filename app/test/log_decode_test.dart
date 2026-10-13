import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart' show makeBlockInfo;
import 'package:tamuapp/ui/log_page.dart';

/// Pins the app's Log Struct decode against the firmware layout
/// (Docs/Services/Log Handler.md): source (the whole block index - BlockInfo
/// type|instance|field|key, u32) | code (source-specific u32) | timestamp (u32). A service log
/// uses the reserved source type 0x3FF with the ServiceType in the instance field.
void main() {
  List<int> entry(int source, int code, int ts) => [
    0, 0, // device id
    0, 0, // count
    source & 0xFF,
    (source >> 8) & 0xFF,
    (source >> 16) & 0xFF,
    (source >> 24) & 0xFF,
    code & 0xFF, (code >> 8) & 0xFF, (code >> 16) & 0xFF, (code >> 24) & 0xFF,
    ts & 0xFF, (ts >> 8) & 0xFF, (ts >> 16) & 0xFF, (ts >> 24) & 0xFF,
  ];

  test('a block log decodes its type, instance and 32-bit code', () {
    final e = LogEntry.fromBytes(
      entry(makeBlockInfo(0x05, 7, 0, 0), 0x1234, 42),
    );
    expect(e.isBlock, isTrue);
    expect(e.sourceType, 0x05);
    expect(e.instance, 7);
    expect(e.sourceId, 0x05);
    expect(e.code, 0x1234);
    expect(e.timestampMs, 42);
  });

  test('a service log decodes the service type from the instance field', () {
    final e = LogEntry.fromBytes(
      entry(makeBlockInfo(0x3FF, 3, 0, 0), 0x00AB, 9),
    );
    expect(e.isBlock, isFalse);
    expect(e.sourceId, 0x03); // ServiceType.storage
    expect(e.code, 0x00AB);
  });
}
