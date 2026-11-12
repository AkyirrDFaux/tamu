import 'package:flutter/material.dart';

import '../core/register_client.dart';
import '../core/types.dart';
import 'theme.dart';
import 'widgets.dart';

/// One field read as the device answered it, or null when it did not answer - the shape
/// [RegisterClient.readBlockField] returns. (`core/ble_security.dart` names the same type
/// [BleFieldRead]; a block panel does not have to be the BLE one to read a field.)
typedef BlockFieldRead = ({ValueInfo meta, List<int> value})?;

/// The up-front line a panel shows about the link this app is on: a mark and one sentence. Returned
/// as a list so the caller can spread it into a column (both block panels draw it the same way, and
/// the sentence is the caller's).
///
/// The defaults are the WiFi panel's shape - the usb mark in the warning ink, because that block
/// takes a write only over USB. A panel whose link may write the block passes its own mark and ink
/// instead of reading as a warning (`BleSecurityPage` does, over a secured Bluetooth link).
List<Widget> linkNotice(
  BuildContext context,
  String message, {
  IconData icon = Icons.usb,
  Color color = kWarning,
}) => [
  const SizedBox(height: Spacing.md),
  Row(
    crossAxisAlignment: CrossAxisAlignment.start,
    children: [
      Icon(icon, size: IconSize.sm, color: color),
      const SizedBox(width: Spacing.sm),
      Expanded(
        child: Text(message, style: Theme.of(context).textTheme.bodySmall),
      ),
    ],
  ),
];

/// The refresh of a settings panel built on ONE device block, and the state behind it.
///
/// Both block panels in the app (`BLE Settings`, `WiFi Settings`) drew their own copy of this
/// and the copies had started to drift in the words they print. The steps are the same and their
/// order matters, so they live here once: enumerate the blocks the device carries, treat a
/// silent enumeration as the failure it is, treat a block that is not in the list as the normal
/// case, read the panel's own block field by field, and publish either the reading or the
/// sentence that says why there is none. What differs between two panels is data - which block,
/// how many fields, how a read becomes the panel's reading, and what the panel calls its block -
/// and the page supplies it.
///
/// The three outcomes stay apart because the DEVICE makes them different:
///
///  - no answer to the enumeration is a failure: the device said nothing about its own blocks,
///  - a block that is not in the enumeration is not a failure: a compile-in block (WiFi) or a
///    core-only one (BLE) is simply absent on a build that does not carry it, and the page then
///    draws nothing at all,
///  - a block that is there and answers no field is a failure again: the device claims a block it
///    will not let anyone read.
mixin BlockPanelRefresh<W extends StatefulWidget, S> on State<W> {
  /// The client this panel asks.
  RegisterClient get blockClient;

  /// Whether this panel's block came back in the device's own [types] enumeration.
  bool hasBlock(Iterable<({int type, int maxInstance})> types);

  /// The block this panel reads.
  int get blockType;

  /// How many fields the block has; they are read in order, `0 .. blockFieldCount - 1`.
  int get blockFieldCount;

  /// The panel's reading of [reads]: one entry per field, null where the device did not answer.
  ///
  /// Called only when at least one field answered, so a reading never has to describe a block
  /// that said nothing at all.
  S snapshotOf(Map<int, BlockFieldRead> reads);

  /// What this panel calls its block in the one sentence a silent block earns ("Bluetooth",
  /// "WiFi").
  String get blockName;

  /// Whether the app is on a live link - the first reason not to ask.
  bool get blockConnected;

  /// Whether a write is in flight: the panel does not read a block it is writing.
  bool get blockBusy;

  /// Runs once a block was read and its reading published, for a panel that has work to do on
  /// one (the BLE panel re-arms its pairing-window countdown).
  void onBlockRead() {}

  S? _snapshot;

  /// The raw field reads behind [_snapshot]: a write path takes a field's declared type and
  /// width from here instead of guessing them.
  Map<int, BlockFieldRead> _reads = const {};

  /// False when the device's enumeration does not carry the block: the page then draws nothing.
  bool _present = true;

  bool _refreshing = false;
  String? _error;

  /// The last reading of the block, or null before the first successful one.
  S? get blockSnapshot => _snapshot;

  /// The field reads behind [blockSnapshot], by field number.
  Map<int, BlockFieldRead> get blockReads => _reads;

  /// False when the device does not carry the block: a page on this mixin draws nothing then.
  bool get blockPresent => _present;

  /// True while a refresh is in flight.
  bool get blockRefreshing => _refreshing;

  /// Why the last refresh has no reading, or null when it has one.
  String? get blockError => _error;

  /// Reads this panel's block and publishes what it found.
  ///
  /// A page that is not on a live link, is already refreshing, or is mid-write does nothing: the
  /// guard is the whole reason a refresh is safe to fire from a write's tail, a timer tick and a
  /// retry button at once.
  Future<void> refreshBlock() async {
    if (!blockConnected || _refreshing || blockBusy) return;
    setState(() => _refreshing = true);
    try {
      final types = await blockClient.enumerateBlockTypes();
      if (!mounted) return;
      if (types == null) {
        setState(() {
          _refreshing = false;
          _error = 'The device did not answer the block enumeration';
        });
        return;
      }
      if (!hasBlock(types)) {
        // No block, no panel: a device that does not carry the block is the normal case, not a
        // failure - nothing is drawn, and the page is not written off as broken.
        setState(() {
          _refreshing = false;
          _present = false;
          _error = null;
          _snapshot = null;
        });
        return;
      }

      final reads = <int, BlockFieldRead>{};
      for (var field = 0; field < blockFieldCount; field++) {
        reads[field] = await blockClient.readBlockField(blockType, 0, field, 0);
      }
      if (!mounted) return;
      final answered = reads.values.any((r) => r != null);
      setState(() {
        _refreshing = false;
        _present = true;
        _reads = reads;
        // Present but silent is a failure worth showing: the device claims a block it will not
        // let anyone read, which is not the same thing as not having one.
        _error = answered
            ? null
            : 'The $blockName block is present but did not answer a read';
        _snapshot = answered ? snapshotOf(reads) : null;
      });
      onBlockRead();
    } finally {
      if (mounted && _refreshing) setState(() => _refreshing = false);
    }
  }

  /// Folds one later field read into the current reading and rebuilds the panel's snapshot from
  /// it - for a panel that re-reads a single field without reading the whole block again.
  void foldBlockRead(int field, BlockFieldRead read) {
    _reads = {..._reads, field: read};
    _snapshot = snapshotOf(_reads);
  }

  /// The page this panel draws while it has no reading, or null when [blockSnapshot] holds one
  /// and the page draws its form.
  ///
  /// The THREE outcomes [refreshBlock] can end on are worded here once, because the two panels
  /// drew the same scaffold separately and the copies drifted:
  ///
  ///  - the device does not carry the block: nothing at all ([TamuPage] with an empty body),
  ///  - a refresh that produced no reading: the reason and a Try again,
  ///  - a panel that has not read yet: the waiting state, naming what is being read.
  ///
  /// [unavailable] is the failure's headline ("WiFi block unavailable"), [reading] the waiting
  /// line ("Reading WiFi settings") - what the panel calls its block in each case.
  Widget? blockPanelStatePage({
    required String title,
    required String unavailable,
    required String reading,
    required Future<void> Function() onRefresh,
    required List<Widget> actions,
  }) {
    if (!blockPresent) return TamuPage(title: title, actions: actions);
    if (blockSnapshot != null) return null;
    final error = blockError;
    if (error != null) {
      return TamuPage(
        title: title,
        actions: actions,
        state: StateKind.error,
        stateMessage: unavailable,
        stateDetails: error,
        stateAction: OutlinedButton(
          onPressed: onRefresh,
          child: const Text('Retry'),
        ),
      );
    }
    return TamuPage(
      title: title,
      actions: actions,
      state: StateKind.loading,
      stateMessage: reading,
    );
  }
}
