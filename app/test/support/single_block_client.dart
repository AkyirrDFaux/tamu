/// A Register client that serves ONE static block, which is all a block-panel row test needs.
///
/// `wifi_register_row_test` and `register_meas_rail_tiles_test` each wrote the same three
/// overrides: an `enumerateBlockTypes` answer naming one block, an empty static-field layout (only
/// the Backup view reads it) and a `readBlockField` switch over the block's field numbers. The
/// first two are the same in every such suite and were the bigger half of both fakes; the switch is
/// the part a suite is actually about, so it stays there.
library;

import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';

/// A page's fake for one static block: [blockType] at [blockInstance], [fieldCount] rows.
abstract class SingleBlockRegisterClient extends RegisterClient {
  SingleBlockRegisterClient({required super.deviceId});

  /// The block's type, as the firmware numbers it (`BlockType`).
  int get blockType;

  /// The instance the block sits at - 0 for every single-instance block.
  int get blockInstance => 0;

  /// The name the block reports; a row test taps it to expand the block.
  String get blockName;

  /// How many fields the block declares, which is the size the block meta reports.
  int get fieldCount;

  @override
  Future<BlocksRead?> readBlocks({List<int>? scriptSlots}) async => (
    blocks: <ReportedBlock>[
      (
        type: blockType,
        inst: blockInstance,
        meta: ValueInfo(type: blockType, size: fieldCount),
        name: blockName,
      ),
    ],
    unreadTypes: const <int>{},
  );

  /// Only the Backup view reads the persistent layout; the Current view renders live reads.
  @override
  Future<StaticFieldLayout> readStaticFieldLayout() async => const {};
}

/// The `ValueInfo` a block field reports: its type, its width and its `ValueFlags`.
ValueInfo fieldInfo(int type, int size, int flags) =>
    ValueInfo(type: type, size: size, flags: flags);
