/// Core types for the hardware-in-the-loop suite - see Docs/HIL Tests.md.
///
/// The suite scans what is on the bus, then runs every test whose prerequisite
/// the rig satisfies; an unmet prerequisite is a skip, never a failure.
library;

import 'package:tamuapp/core/types.dart';

import 'hil_log.dart';

/// Where a test belongs (Docs/HIL Tests.md, "Test Categories").
enum HilCategory { generic, service, module, bootloader, partner }

/// When a test may run relative to the others (Docs/HIL Tests.md,
/// "Independence and Ordering").
///
/// `independent` tests run in any order and in parallel; `exclusive` tests own
/// their device alone (the bootloader halts the application); `coupled` tests
/// change network-wide state and run one at a time after the independent set.
enum HilRunClass { independent, exclusive, coupled }

/// A prerequisite the rig satisfies or not (Docs/HIL Tests.md, "Prerequisites").
class HilPrereq {
  /// Role: `true` needs the core, `false` needs a node, `null` accepts either.
  final bool? isCore;

  /// Capability bits the device advertises; every bit is required.
  final List<int> capabilities;

  /// Block types the device exposes; every type is required - the modules.
  final List<BlockType> blocks;

  /// The device answers the bootloader (declared by the operator).
  final bool bootloader;

  /// Another device that satisfies this prerequisite; the test needs a partner.
  final HilPrereq? partner;

  /// The rig holds at least this many devices that satisfy this prerequisite.
  final int peers;

  const HilPrereq({
    this.isCore,
    this.capabilities = const [],
    this.blocks = const [],
    this.bootloader = false,
    this.partner,
    this.peers = 0,
  });

  @override
  String toString() {
    final parts = <String>[
      if (isCore == true) 'core',
      if (isCore == false) 'node',
      for (final bit in capabilities) 'cap:0x${bit.toRadixString(16)}',
      for (final block in blocks) 'block:${block.label}',
      if (bootloader) 'bootloader',
      if (partner != null) 'partner($partner)',
      if (peers > 0) 'peers>=$peers',
    ];
    return parts.isEmpty ? 'any device' : parts.join(' + ');
  }
}

/// One device on the bus, resolved from the scan.
class HilDevice {
  final int id;
  final DeviceType type;
  final int capabilities;
  final String name;
  final List<BlockType> blocks;
  final bool bootloader;

  const HilDevice({
    required this.id,
    required this.type,
    required this.capabilities,
    required this.name,
    this.blocks = const [],
    this.bootloader = false,
  });

  bool get isCore => capabilities & Capability.core != 0;
  bool hasCapability(int bit) => capabilities & bit != 0;
  bool hasBlock(BlockType block) => blocks.contains(block);

  /// `Type #id`, for test names and the run log.
  String get label => '${type.label} #$id';

  @override
  String toString() =>
      '$label '
      '[caps 0x${capabilities.toRadixString(16)}; '
      'blocks ${blocks.map((b) => b.label).join(",")}'
      '${bootloader ? "; bootloader" : ""}]';
}

/// What the scan found (Docs/HIL Tests.md, "Rig Inventory").
class HilRig {
  final List<HilDevice> devices;
  const HilRig(this.devices);

  bool get isEmpty => devices.isEmpty;

  /// Every device whose own attributes satisfy [prereq] (the rig-wide partner
  /// and peer clauses excluded).
  List<HilDevice> satisfying(HilPrereq prereq) =>
      devices.where((d) => _deviceSatisfies(d, prereq)).toList();

  /// A device other than [exclude] that satisfies [prereq], or null.
  HilDevice? partnerFor(HilPrereq prereq, {HilDevice? exclude}) {
    for (final d in devices) {
      if (identical(d, exclude)) continue;
      if (_deviceSatisfies(d, prereq)) return d;
    }
    return null;
  }

  bool _deviceSatisfies(HilDevice device, HilPrereq prereq) {
    if (prereq.isCore != null && device.isCore != prereq.isCore) return false;
    for (final bit in prereq.capabilities) {
      if (!device.hasCapability(bit)) return false;
    }
    for (final block in prereq.blocks) {
      if (!device.hasBlock(block)) return false;
    }
    if (prereq.bootloader && !device.bootloader) return false;
    return true;
  }

  /// The rig-wide clauses: partner and peers. Null when they hold.
  String? _unmetRigWide(HilPrereq prereq, List<HilDevice> candidates) {
    if (prereq.partner != null) {
      final needsPartner = candidates.every(
        (d) => partnerFor(prereq.partner!, exclude: d) == null,
      );
      if (needsPartner) return 'no partner device for $prereq';
    }
    if (prereq.peers > 0 && satisfying(prereq).length < prereq.peers) {
      return 'needs ${prereq.peers} matching device(s), found ${satisfying(prereq).length}';
    }
    return null;
  }

  /// The unmet prerequisite as a skip reason, or null when the rig satisfies it.
  String? unmet(HilPrereq prereq) {
    final candidates = satisfying(prereq);
    if (candidates.isEmpty) {
      final name = prereq.toString();
      if (prereq.isCore == true && devices.every((d) => !d.isCore)) {
        return 'no core on the bus';
      }
      if (prereq.isCore == false && devices.every((d) => d.isCore)) {
        return 'no node on the bus';
      }
      return 'no device with $name';
    }
    return _unmetRigWide(prereq, candidates);
  }

  /// A one-line inventory per device, for the log.
  List<String> describe() => devices.map((d) => '  ${d.id}\t$d').toList();
}

/// What a test body receives: the rig, the log and helpers to pick devices.
class HilContext {
  final HilRig rig;
  final HilLog log;
  final HilTest test;

  HilContext(this.rig, this.log, this.test);

  /// The first device the prerequisite accepts. The runner guarantees one
  /// exists before the body runs.
  HilDevice device(HilPrereq prereq) => rig.satisfying(prereq).first;

  /// Every device the prerequisite accepts, in scan order.
  List<HilDevice> devices(HilPrereq prereq) => rig.satisfying(prereq);

  void note(String text) => log.line('    $text');
}

/// One test definition (Docs/HIL Tests.md, "Adding a Test").
class HilTest {
  final String name;
  final HilCategory category;
  final HilRunClass runClass;
  final HilPrereq prereq;
  final Future<void> Function(HilContext ctx) body;

  const HilTest(
    this.name, {
    required this.category,
    this.runClass = HilRunClass.independent,
    this.prereq = const HilPrereq(),
    required this.body,
  });

  String get label => '[${category.name}] $name  <-  $prereq';
}
