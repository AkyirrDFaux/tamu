import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/ui/log_page.dart';

/// Pins the app's Log Struct decode against the firmware layout
/// (Docs/Services/Log Handler.md): source (BlockInfo type|instance, u16) | category (u8) |
/// specifics (u8). A service log uses the reserved source type 0x3FF with the ServiceType in
/// the instance field.
void main() {
  List<int> entry(int source, int category, int specifics, int ts) => [
        0, 0, // device id
        0, 0, // count
        source & 0xFF, (source >> 8) & 0xFF, category, specifics,
        ts & 0xFF, (ts >> 8) & 0xFF, (ts >> 16) & 0xFF, (ts >> 24) & 0xFF,
      ];

  test('a block log decodes its type, instance and 16-bit code', () {
    final e = LogEntry.fromBytes(entry(0x1C05, 0x12, 0x34, 42));
    expect(e.isBlock, isTrue);
    expect(e.sourceType, 0x05);
    expect(e.instance, 7);
    expect(e.sourceId, 0x05);
    expect(e.code, 0x1234);
    expect(e.timestampMs, 42);
  });

  test('a service log decodes the service type from the instance field', () {
    final e = LogEntry.fromBytes(entry(0x0FFF, 0x00, 0xAB, 9));
    expect(e.isBlock, isFalse);
    expect(e.sourceId, 0x03); // ServiceType.storage
    expect(e.code, 0x00AB);
  });
}
