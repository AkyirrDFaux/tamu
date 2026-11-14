/// A fake v2 bootloader device, shared by [BootloaderClient]'s own suite and the Update page's.
///
/// The fake IS the device: one [space] of [appSize] bytes, erased to 0xFF, that behaves like NOR
/// flash - an erase blanks a page, a write only clears bits (`space[i] &= data[i]`), and a write
/// whose stored bytes do not read back as sent is refused, exactly as the bootloader's ack rule
/// says. So a client that writes without erasing is caught here by the same rule the node applies,
/// and not by a mock's opinion.
///
/// Each fault the client has to tell apart has its own switch: the four passthrough reasons
/// (busy / malformed / node silent / node refused), a write the channel accepts but the device
/// loses, a read path that lies while the flash is right, a page the node will not blank, a
/// pre-v2 identify body, and a bootloader with no CRC command. [beforeCrc] is the bench hook a
/// corruption test needs: it runs between the CRC and the read-back, so a test can damage one word
/// "mid-verify" the way an SWD write does.
library;

import 'dart:typed_data';

import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';

class FakeBootloaderTransport implements BootloaderTransport {
  FakeBootloaderTransport({
    this.appSize = 1024,
    this.pageSize = 64,
    this.name = 'DAS v0.1',
  }) : space = Uint8List(appSize)..fillRange(0, appSize, 0xFF);

  /// The app space the device reports.
  final int appSize;

  /// The erase page size the device reports.
  final int pageSize;

  /// The name the device reports.
  final String name;

  /// What the device holds. Erased (0xFF) until something is written.
  final Uint8List space;

  /// Pages whose erase the node refuses: its storage region, or a write-protected page.
  final Set<int> protectedPages = {};

  /// Offsets whose next write the node refuses with its own failure frame.
  final Set<int> rejectWriteOnce = {};

  /// Offsets whose next write is acknowledged and then LOST: the device accepted it and the bytes
  /// are not there, which is what the read-back exists to find.
  final Set<int> dropWriteOnce = {};

  /// Offsets whose every write is stored with its first byte flipped: the flash does not hold
  /// what it is given, so no correction round can settle it.
  final Set<int> corruptWrite = {};

  /// True to damage every write once, so a single correction round can fix it.
  bool corruptFirstWriteOnly = false;

  /// Offsets whose READ comes back with a byte flipped: the stored bytes and the CRC are right,
  /// the read path is not - the node's own CRC and its own bytes disagree.
  bool readLies = false;

  /// True when the device answers nothing (`0x03`) to every request.
  bool silent = false;

  /// How many more requests are refused as busy (`0x01`) before the device answers.
  int busyLeft = 0;

  /// True to refuse every request as busy, for the exhausted-backoff case.
  bool busyAlways = false;

  /// True to refuse every request as malformed (`0x02`), i.e. a client bug.
  bool malformed = false;

  /// True to model a FAIL with no reason byte: a core older than the reason table.
  bool unreported = false;

  /// True to model a pre-v2 identify body: the page field is the first four bytes of the name.
  bool oldBootloader = false;

  /// True when the node refuses the CRC request (a bootloader without the CRC command).
  bool noCrc = false;

  /// Runs between the CRC exchange and the read-back of one verification, like an SWD write
  /// landing mid-verify. The second argument is the verification's 1-based round number.
  void Function(FakeBootloaderTransport device, int round)? beforeReadBack;

  int identifies = 0;
  int reads = 0;
  int writes = 0;
  int erases = 0;
  int crcs = 0;

  /// Verification rounds started (one per CRC exchange).
  int rounds = 0;

  final Set<int> _programmed = {};

  /// The bytes the device should hold for [image] in an app space of [appSize]: the image, 0xFF
  /// past its end, with any protected page left as it is. What a clean upload must produce.
  Uint8List expectedSpace(List<int> image) {
    final out = Uint8List(appSize)..fillRange(0, appSize, 0xFF);
    final n = image.length > appSize ? appSize : image.length;
    out.setRange(0, n, image);
    for (final page in protectedPages) {
      out.setRange(page, page + pageSize, space.sublist(page, page + pageSize));
    }
    return out;
  }

  /// True when the device holds exactly [expectedSpace] for [image].
  bool holds(List<int> image) {
    final want = expectedSpace(image);
    for (var i = 0; i < appSize; i++) {
      if (space[i] != want[i]) return false;
    }
    return true;
  }

  void _gate(String what) {
    if (busyAlways) {
      throw PassthroughFailure(PassthroughReason.busy, what);
    }
    if (busyLeft > 0) {
      busyLeft--;
      throw PassthroughFailure(PassthroughReason.busy, what);
    }
    if (malformed) {
      throw PassthroughFailure(PassthroughReason.malformed, what, code: 0x02);
    }
    if (unreported) {
      throw PassthroughFailure(PassthroughReason.unreported, what);
    }
    if (silent) {
      throw PassthroughFailure(PassthroughReason.nodeSilent, what, code: 0x03);
    }
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async {
    identifies++;
    // The transport contract: a node that answers nothing to identify gives a NULL identity (it is
    // not in bootloader mode) rather than a failure - the other requests still report silence.
    if (silent) return null;
    _gate('identify');
    // A pre-v2 body has the name where the page size belongs: the first four name characters read
    // as the page size, which is what the real one does.
    final v1Page = name.length >= 4
        ? name.codeUnitAt(0) |
              (name.codeUnitAt(1) << 8) |
              (name.codeUnitAt(2) << 16) |
              (name.codeUnitAt(3) << 24)
        : 0;
    return BootloaderIdentity(
      appSize: appSize,
      pageSize: oldBootloader ? v1Page : pageSize,
      name: name,
    );
  }

  @override
  Future<Uint8List> readChunk(int offset, Duration timeout) async {
    reads++;
    _gate('read 0x${offset.toRadixString(16)}');
    final data = Uint8List.fromList(
      space.sublist(offset, offset + Bootloader.payloadSize),
    );
    if (readLies) data[1] = data[1] ^ 0xFF;
    return data;
  }

  @override
  Future<int> erasePage(int offset, Duration timeout) async {
    erases++;
    _gate('erase 0x${offset.toRadixString(16)}');
    if (offset % pageSize != 0 || offset >= appSize) {
      throw PassthroughFailure(
        PassthroughReason.nodeRefused,
        'erase 0x${offset.toRadixString(16)}',
        code: 0x04,
      );
    }
    if (protectedPages.contains(offset)) {
      throw PassthroughFailure(
        PassthroughReason.nodeRefused,
        'erase 0x${offset.toRadixString(16)}',
        code: 0x04,
      );
    }
    space.fillRange(offset, offset + pageSize, 0xFF);
    _programmed.removeWhere((o) => o >= offset && o < offset + pageSize);
    return offset;
  }

  @override
  Future<WriteAck> writeChunk(
    int offset,
    List<int> data,
    Duration timeout,
  ) async {
    writes++;
    _gate('write 0x${offset.toRadixString(16)}');
    if (rejectWriteOnce.remove(offset)) {
      throw PassthroughFailure(
        PassthroughReason.nodeRefused,
        'write 0x${offset.toRadixString(16)}',
        code: 0x04,
      );
    }
    if (dropWriteOnce.remove(offset)) {
      // Accepted, acknowledged - and never programmed.
      return WriteAck(true, acked: offset);
    }
    if (offset % pageSize == 0 && !_blank(offset, 4)) {
      // The firmware's page-start rule: a chunk that starts a page is refused (naming the page)
      // when the page's first word is not blank, i.e. the tool wrote without erasing.
      return WriteAck(false, acked: (offset & ~(pageSize - 1)) | 1);
    }
    // NOR flash: programming only clears bits, so a dirty byte stays dirty and the ack says so.
    var landed = true;
    final corrupting =
        corruptWrite.contains(offset) &&
        (!corruptFirstWriteOnly || !_programmed.contains(offset));
    for (var i = 0; i < Bootloader.payloadSize; i++) {
      space[offset + i] &= data[i];
      if (space[offset + i] != data[i]) landed = false;
    }
    if (landed && corrupting) {
      // The device took the bytes and the flash did not keep them. The ack still names the chunk,
      // so only the read-back can find this.
      space[offset] = space[offset] ^ 0xFF;
    }
    _programmed.add(offset);
    return landed
        ? WriteAck(true, acked: offset)
        : WriteAck(false, acked: offset);
  }

  @override
  Future<int> crc(Duration timeout) async {
    crcs++;
    rounds++;
    _gate('CRC');
    if (noCrc) {
      throw const PassthroughFailure(
        PassthroughReason.nodeRefused,
        'CRC',
        code: 0x04,
      );
    }
    final hook = beforeReadBack;
    if (hook != null) hook(this, rounds);
    // The node computes the CRC over its WHOLE app space, from flash, not through the read path.
    return Bootloader.crc32(space);
  }

  bool _blank(int offset, int length) {
    for (var i = 0; i < length; i++) {
      if (space[offset + i] != 0xFF) return false;
    }
    return true;
  }
}

/// A test image: `length` bytes of `i & 0xFF`.
Uint8List testImage(int length) =>
    Uint8List.fromList(List.generate(length, (i) => i & 0xFF));
