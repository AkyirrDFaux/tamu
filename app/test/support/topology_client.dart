/// The Register client the page suites mount: one System block, answered slowly or not at all.
///
/// Three suites (`loading_states`, `loading_exits`, `table_read_failure_page`) each declared their
/// own `_TopologyClient extends RegisterClient` with the same one-block answer and the same empty
/// static-field layout; the copies differed only in which knob they happened to have - one a delay,
/// one a `fail`, one neither. A page test's topology is not its subject, so it lives here once and
/// a suite sets the knob it measures.
library;

import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';

/// A Register client that answers the topology a page needs - one System block of nine fields -
/// after [delay], or not at all when [fail] is set.
class TopologyClient extends RegisterClient {
  TopologyClient({
    required super.deviceId,
    this.delay = Duration.zero,
    this.fail = false,
  });

  /// How long the block read takes; a page test that asserts its wait needs this to be non-zero.
  final Duration delay;

  /// Answer the block read with nothing, as a device that did not reply does.
  final bool fail;

  @override
  Future<BlocksRead?> readBlocks({List<int>? scriptSlots}) async {
    if (delay > Duration.zero) await Future<void>.delayed(delay);
    if (fail) return null;
    return (
      blocks: <ReportedBlock>[
        (
          type: 0,
          inst: 0,
          meta: const ValueInfo(type: 0, size: 9),
          name: 'System',
        ),
      ],
      unreadTypes: const <int>{},
    );
  }

  @override
  Future<StaticFieldLayout> readStaticFieldLayout() async => const {};
}
