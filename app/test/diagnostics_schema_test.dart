import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/diagnostics.dart';
import 'package:tamuapp/core/system_schema.dart';

/// Covers the two small shared modules that had no test: the diagnostics ring (the fallback
/// record for anything that would otherwise vanish into `debugPrint`) and the System block
/// schema (the single source for the Register view, the backup tool and the memory viewers).
void main() {
  group('AppDiagnostics', () {
    test('keeps the newest events, oldest first, capped at capacity', () {
      // The ring is process-global; fill past the cap and assert the surviving window.
      for (var i = 0; i < AppDiagnostics.capacity + 25; i++) {
        AppDiagnostics.log('t', 'event $i');
      }
      final events = AppDiagnostics.events;
      expect(events.length, AppDiagnostics.capacity);
      expect(events.first.message, 'event 25');
      expect(events.last.message, 'event ${AppDiagnostics.capacity + 24}');
      // Oldest first.
      expect(events.first.time.isAfter(events.last.time), isFalse);
    });

    test('events render as "<time> [source] message"', () {
      AppDiagnostics.log('link', 'hello');
      final line = AppDiagnostics.events.last.toString();
      expect(line, contains('[link] hello'));
      // The time is formatted as a wall clock (substring of the ISO string).
      expect(line, matches(RegExp(r'^\d\d:\d\d:\d\d\.\d\d\d \[')));
    });

    test('dump joins every event with newlines', () {
      final dump = AppDiagnostics.dump();
      expect(dump.split('\n').length, AppDiagnostics.events.length);
    });
  });

  group('system schema', () {
    test('every System field has a name and at least one key', () {
      for (var field = 0; field < systemFieldCount; field++) {
        expect(systemFieldName(field), isNot('Field $field'),
            reason: 'field $field is unnamed');
        expect(systemKeysForField(field), isNotEmpty);
      }
    });

    test('key names resolve for every declared key', () {
      for (final entry in systemFieldKeys.entries) {
        for (final key in entry.value.keys) {
          expect(systemKeyName(entry.key, key), isNot('Key $key'),
              reason: 'field ${entry.key} key $key is unnamed');
        }
      }
    });

    test('an unknown field falls back rather than throwing', () {
      expect(systemFieldName(99), 'Field 99');
      expect(systemKeyName(99, 0), 'Key 0');
      expect(systemStructMemberName(99, 0), 'Key 0');
      expect(systemKeysForField(99), [0]);
    });
  });
}
