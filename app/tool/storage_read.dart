/// Live rig run of the storage read path and the app's cold-start request phases.
///
/// Why: two behaviours are only measurable on the wire - (a) whether a whole-file read arrives
/// complete (the `.TABLE  ` reply is 256 fragments over a 8 kB transmit ring, and over BLE the
/// core drops frames), and (b) what the app's cold start costs request by request. Both are
/// measured here through the APP'S OWN clients (`StorageClient`, `RegisterClient`, `DeviceDatabase`
/// readers) over the same framing the app uses, so the numbers belong to the code under test and
/// not to a copy of it.
///
/// Usage (from `app/`, needs the rig's core on USB serial):
///
///     dart run tool/storage_read.dart
///     dart run tool/storage_read.dart --capture <path>     # bank the .TABLE stream as a fixture
///     dart run tool/storage_read.dart --no-startup         # reads only
///
/// The `--capture` file is the packet-stream bytes of the `.TABLE  ` reply exactly as the app's
/// parser receives them (USB link frames already unwrapped), which is what
/// `test/storage_partial_reply_test.dart` replays.
library;

import 'dart:async';
import 'dart:io';

import 'package:tamuapp/core/device_identity.dart';
import 'package:tamuapp/core/diagnostics.dart';
import 'package:tamuapp/core/protocol.dart' show ServiceType;
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/sndb.dart';
import 'package:tamuapp/core/service_request.dart';
import 'package:tamuapp/core/storage_client.dart';

import 'rig_link.dart';

/// One measured phase: what it was, how long it took and how many requests it sent.
class _Phase {
  final String name;
  final int micros;
  final int requests;
  const _Phase(this.name, this.micros, this.requests);
}

Future<_Phase> _time(
  String name,
  Future<void> Function() body, {
  int requests = 0,
}) async {
  final watch = Stopwatch()..start();
  await body();
  watch.stop();
  return _Phase(name, watch.elapsedMicroseconds, requests);
}

String _ms(int micros) => (micros / 1000).toStringAsFixed(1);

void _printPhase(_Phase phase) {
  stdout.writeln(
    '${phase.name.padRight(42)} ${_ms(phase.micros).padLeft(9)} ms'
    '  ${phase.requests.toString().padLeft(3)} req',
  );
}

/// Prints the diagnostic lines a block of work produced, so the retry log lines the card quotes
/// are the app's own (`AppDiagnostics`), not a summary printed next to them.
void _drainDiagnostics(Set<String> seen) {
  for (final event in AppDiagnostics.events) {
    final line = event.toString();
    if (seen.add(line)) stdout.writeln('  log  $line');
  }
}

/// Waits for the core to answer a ping.
///
/// Opening the port asserts DTR/RTS, which resets the core, so the first pings are expected to
/// be dropped - the app's own connect path waits the same way.
Future<bool> _ping(RigChannel link) async {
  final deadline = DateTime.now().add(const Duration(seconds: 10));
  while (DateTime.now().isBefore(deadline)) {
    final reply = await serviceRequest(
      1,
      ServiceType.device,
      1,
      timeout: const Duration(milliseconds: 800),
      tag: 'ping',
    );
    if (reply != null) return true;
    if (!link.isConnected) return false;
  }
  return false;
}

/// The layout file's header, decoded the way the app's viewer decodes it
/// (`file_viewers.dart` `_layoutView`): u8 brightness limit, u8 width, u8 height, then
/// width*height little-endian LED indexes.
({int limit, int width, int height, int leds})? _layoutHeader(List<int> bytes) {
  if (bytes.length < 3) return null;
  final limit = bytes[0];
  final width = bytes[1];
  final height = bytes[2];
  if (width == 0 || height == 0 || 3 + width * height * 2 > bytes.length) {
    return null;
  }
  var leds = 0;
  for (var at = 3; at + 1 < bytes.length; at += 2) {
    if ((bytes[at] | (bytes[at + 1] << 8)) != 0xFFFF) leds++;
  }
  return (limit: limit, width: width, height: height, leds: leds);
}

Future<void> main(List<String> args) async {
  var capturePath = '';
  var startup = true;
  for (var i = 0; i < args.length; i++) {
    if (args[i] == '--capture' && i + 1 < args.length) {
      capturePath = args[i + 1];
    }
    if (args[i] == '--no-startup') startup = false;
  }

  preloadLibserialport();
  final ports = corePortCandidates();
  if (ports.isEmpty) {
    stderr.writeln('no serial port: is the core plugged in?');
    exitCode = 2;
    return;
  }
  stdout.writeln('port: ${ports.first}');
  final link = await RigChannel.open(ports.first);
  installRequestChannel(link);
  final seen = <String>{};
  try {
    final phases = <_Phase>[];

    // ---- cold start: the requests the app sends before its first usable frame -----------------
    if (startup) {
      final pingWatch = Stopwatch()..start();
      final answered = await _ping(link);
      pingWatch.stop();
      if (!answered) {
        stderr.writeln('the core did not answer a ping');
        exitCode = 3;
        return;
      }
      stdout.writeln(
        '\n=== cold start (requests as `dart run`, one serial link) ===',
      );
      stdout.writeln(
        '${'link open + core ping'.padRight(42)} '
        '${_ms(pingWatch.elapsedMicroseconds).padLeft(9)} ms    1 req',
      );

      DeviceIdentity? identity;
      phases.add(
        await _time('core identity (System fields, batched)', () async {
          identity = await readDeviceIdentity(1);
        }, requests: 6),
      );

      List<({int id, String serial})> roster = const [];
      phases.add(
        await _time('SNDB roster', () async {
          roster = await readSndbEntries() ?? const [];
        }, requests: 1),
      );

      for (final id in {
        1,
        for (final entry in roster)
          if (entry.id != 0) entry.id,
      }) {
        final deviceId = id;
        phases.add(
          await _time('device $deviceId identity + liveness', () async {
            await readDeviceIdentity(deviceId);
          }, requests: 6),
        );
      }

      phases.add(
        await _time('register: block enumeration', () async {
          await RegisterClient(deviceId: 1).readBlocks(
            scriptSlots: await ScriptClient(deviceId: 1).loadedScripts(),
          );
        }, requests: 2),
      );

      for (final phase in phases) {
        _printPhase(phase);
      }
      stdout.writeln(
        'TOTAL request phases                          '
        '${_ms(phases.fold(0, (sum, p) => sum + p.micros)).padLeft(9)} ms',
      );
      stdout.writeln(
        'core: ${identity?.name} '
        'type ${identity?.type.name} '
        'version ${identity?.softwareVersion}',
      );
      stdout.writeln(
        'no file-table read is on the cold-start path: the table belongs to the Storage, '
        'Scripts and Backup views, and only they ask for it.',
      );
      seen.addAll(AppDiagnostics.events.map((e) => e.toString()));
    }

    // ---- storage reads: the file table and the two LED layouts ("emotes") --------------------
    stdout.writeln(
      "\n=== storage reads (StorageClient, the app's own read policy) ===",
    );
    final tableClient = StorageClient(deviceId: 1);
    final tableWatch = Stopwatch()..start();
    List<int>? captured;
    void onBytes(List<int> bytes) => captured = [...?captured, ...bytes];
    if (capturePath.isNotEmpty) link.capture = onBytes;
    final table = await tableClient.readFileTable();
    tableWatch.stop();
    link.capture = null;
    _drainDiagnostics(seen);
    stdout.writeln(
      'file table: ${table?.length ?? 0} record(s) in '
      '${_ms(tableWatch.elapsedMicroseconds)} ms, '
      'attempts ${tableClient.lastReadAttempts}/${StorageClient.readAttempts}'
      '${tableClient.lastReadError == null ? '' : ', error: ${tableClient.lastReadError}'}',
    );
    if (capturePath.isNotEmpty) {
      final bytes = captured ?? const <int>[];
      File(capturePath).writeAsBytesSync(bytes);
      stdout.writeln(
        'captured ${bytes.length} packet-stream byte(s) of the .TABLE reply to $capturePath',
      );
    }
    if (table == null) {
      stderr.writeln(
        'the file table could not be read: ${tableClient.lastReadError}',
      );
      exitCode = 4;
      return;
    }
    for (final record in table) {
      stdout.writeln(
        '  ${normalizeFileName(record.name).padRight(10)} '
        '${record.size.toString().padLeft(6)} B  offset 0x'
        '${record.offset.toRadixString(16).padLeft(8, '0')}',
      );
    }

    // The emote editor's data path: the layouts the file table names, read whole and decoded
    // the way the viewer decodes them.
    for (final record in table) {
      final name = normalizeFileName(record.name).toUpperCase();
      if (!name.startsWith('LAY_')) continue;
      final client = StorageClient(deviceId: 1);
      final watch = Stopwatch()..start();
      final bytes = await client.readFile(
        normalizeFileName(record.name),
        size: record.size,
      );
      watch.stop();
      _drainDiagnostics(seen);
      final header = bytes == null ? null : _layoutHeader(bytes);
      stdout.writeln(
        'emote ${normalizeFileName(record.name).padRight(6)}: '
        '${bytes?.length ?? 0} of ${record.size} byte(s) in '
        '${_ms(watch.elapsedMicroseconds)} ms, '
        'attempts ${client.lastReadAttempts}/${StorageClient.readAttempts}, '
        '${header == null ? 'not decodable (${client.lastReadError})' : '${header.width}x${header.height} LEDs, '
                  'limit ${header.limit}, ${header.leds} lit cells'}',
      );
    }
  } finally {
    installRequestChannel(null);
    await link.close();
  }
}
