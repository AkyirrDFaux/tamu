import 'dart:async';

import 'package:flutter/material.dart';

import '../core/types.dart';

/// Autorefresh plumbing shared by the periodic-refresh pages (Docs/App/
/// Connection.md + Service views): owns the timer, remembers the selected
/// interval, and (for shell-tab pages) pauses while the tab is hidden.
/// Pages override [onAutoRefresh] for the tick and [shellTabIndex] when they
/// are a shell tab (route pages leave it null and keep refreshing while open).
mixin AutoRefreshMixin<T extends StatefulWidget> on State<T> {
  Timer? _autoTimer;
  Duration? _autoInterval;

  /// Index of this page's shell tab, or null for pushed route pages.
  int? get shellTabIndex => null;

  /// One autorefresh tick: re-read the page's data.
  Future<void> onAutoRefresh() async {}

  /// Runs once when autorefresh is turned on (e.g. an immediate refresh).
  void onAutoRefreshStarted() {}

  @override
  void initState() {
    super.initState();
    if (shellTabIndex != null) ShellTabs.instance.addListener(_onTabChanged);
  }

  @override
  void dispose() {
    if (shellTabIndex != null) {
      ShellTabs.instance.removeListener(_onTabChanged);
    }
    _autoTimer?.cancel();
    super.dispose();
  }

  bool get autoRefreshActive => _autoTimer != null;
  Duration? get selectedInterval => _autoInterval;

  void _onTabChanged() {
    final tab = shellTabIndex;
    if (tab == null) return;
    if (ShellTabs.instance.index == tab) {
      applyAuto(_autoInterval); // tab visible again: restore the remembered interval
    } else {
      _autoTimer?.cancel(); // tab hidden: stop but keep the remembered interval
      _autoTimer = null;
      if (mounted) setState(() {});
    }
  }

  /// Enables/disables autorefresh; null or [Duration.zero] disables.
  void applyAuto(Duration? interval) {
    _autoTimer?.cancel();
    _autoTimer = null;
    setState(() =>
        _autoInterval = (interval == null || interval == Duration.zero) ? null : interval);
    if (_autoInterval != null) {
      _autoTimer = Timer.periodic(_autoInterval!, (_) => onAutoRefresh());
      onAutoRefreshStarted();
    }
  }
}

/// Formats a millisecond duration as `Hh Mm Ss` (shared by the Device view
/// uptime and the Log entry timestamps).
String formatUptimeMs(int ms) {
  final seconds = ms ~/ 1000;
  final h = seconds ~/ 3600;
  final m = (seconds % 3600) ~/ 60;
  final s = seconds % 60;
  return '${h}h ${m}m ${s}s';
}

/// Name + block-type prompt shared by the Dynamic/Keyed memory create and edit
/// flows. With `withIndex` the user may pin the new block to an explicit index
/// (filling a None placeholder); an empty index appends.
/// Returns (name, type, index) or null when cancelled.
Future<(String, BlockType, int?)?> promptBlockNameAndType(
  BuildContext context, {
  String initialName = '',
  BlockType? initialType,
  required String title,
  bool withIndex = false,
}) async {
  final nameController = TextEditingController(text: initialName);
  final indexController = TextEditingController();
  BlockType selected = initialType ?? BlockType.undefined;
  return await showDialog<(String, BlockType, int?)>(
    context: context,
    builder: (context) => StatefulBuilder(
      builder: (context, setState) => AlertDialog(
        title: Text(title),
        content: Column(mainAxisSize: MainAxisSize.min, children: [
          TextField(
            controller: nameController,
            autofocus: true,
            maxLength: 16,
            decoration: const InputDecoration(labelText: 'Block name'),
          ),
          if (withIndex) ...[
            const SizedBox(height: 8),
            TextField(
              controller: indexController,
              keyboardType: TextInputType.number,
              decoration: const InputDecoration(
                  labelText: 'Index (empty = append)',
                  helperText: 'Fills a deleted (None) slot when given'),
            ),
          ],
          const SizedBox(height: 8),
          DropdownButtonFormField<BlockType>(
            initialValue: selected,
            decoration: const InputDecoration(labelText: 'Block type'),
            items: [
              for (final t in BlockType.values)
                if (t != BlockType.deleted)
                  DropdownMenuItem(value: t, child: Text(t.label)),
            ],
            onChanged: (t) => setState(() => selected = t ?? BlockType.undefined),
          ),
        ]),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context),
              child: const Text('Cancel')),
          FilledButton(
            onPressed: () {
              final name = nameController.text.trim();
              if (name.isEmpty) return;
              final idxText = indexController.text.trim();
              final idx = idxText.isEmpty ? null : int.tryParse(idxText);
              if (idxText.isNotEmpty && idx == null) return;
              Navigator.pop(context, (name, selected, idx));
            },
            child: const Text('OK'),
          ),
        ],
      ),
    ),
  );
}

/// Tracks which shell tab is currently visible. Pages hosting periodic work
/// (autorefresh) listen to this and pause it whenever their tab is hidden,
/// per the requirement that autorefresh shuts off when the page "closes".
class ShellTabs extends ChangeNotifier {
  ShellTabs._();
  static final ShellTabs instance = ShellTabs._();

  int _index = 0;
  int get index => _index;

  void update(int i) {
    if (_index == i) return;
    _index = i;
    notifyListeners();
  }
}

/// Shared refresh button (Docs/App/Connection.md + Service views):
/// - short press: one immediate refresh
/// - long press: menu to select an autorefresh interval for this page;
///   autorefresh stops when the page closes (callers own the timer).
/// Shows a green dot while autorefresh is active, red on refresh errors.
class RefreshButton extends StatelessWidget {
  final Future<void> Function() onRefresh;
  final bool autoActive;
  final bool refreshing;
  final bool error;

  /// Currently selected interval, null = off.
  final Duration? selectedInterval;
  final ValueChanged<Duration?> onSelectAuto;

  /// Offered intervals in the long-press menu (docs: memory views default
  /// 0.5 s, connection page 1 s; both lists include Off and longer steps).
  final List<(Duration?, String)> intervals;

  const RefreshButton({
    super.key,
    required this.onRefresh,
    required this.autoActive,
    required this.refreshing,
    required this.error,
    required this.selectedInterval,
    required this.onSelectAuto,
    this.intervals = const [
      (null, 'Off'),
      (Duration(milliseconds: 500), '0.5 s'),
      (Duration(seconds: 1), '1 s'),
      (Duration(seconds: 2), '2 s'),
      (Duration(seconds: 5), '5 s'),
      (Duration(seconds: 10), '10 s'),
    ],
  });

  Future<void> _showMenu(BuildContext context) async {
    final selected = await showDialog<Duration?>(
      context: context,
      builder: (context) => SimpleDialog(
        title: const Text('Autorefresh'),
        children: [
          for (final (interval, label) in intervals)
            SimpleDialogOption(
              // "Off" pops Duration.zero (not null) so it stays distinct from a
              // dismissal, which pops null and must NOT change the setting.
              onPressed: () =>
                  Navigator.pop(context, interval ?? Duration.zero),
              child: Row(children: [
                Text(label),
                const Spacer(),
                if ((interval ?? Duration.zero) ==
                    (selectedInterval ?? Duration.zero))
                  const Padding(
                      padding: EdgeInsets.only(left: 8),
                      child:
                          Icon(Icons.check, size: 16, color: Colors.greenAccent)),
              ]),
            ),
        ],
      ),
    );
    if (!context.mounted || selected == null) {
      return; // dismissed: leave the current setting untouched
    }
    onSelectAuto(selected);
  }

  @override
  Widget build(BuildContext context) {
    final dotColor = error
        ? Colors.redAccent
        : autoActive
            ? Colors.greenAccent
            : Colors.transparent;
    return IconButton(
      tooltip: 'Refresh (hold for autorefresh)',
      onPressed: refreshing ? null : onRefresh,
      onLongPress: () => _showMenu(context),
      icon: Stack(
        clipBehavior: Clip.none,
        children: [
          refreshing
              ? const SizedBox(
                  width: 18,
                  height: 18,
                  child: CircularProgressIndicator(strokeWidth: 2))
              : const Icon(Icons.refresh),
          Positioned(
            right: -2,
            bottom: -2,
            child: Icon(Icons.circle, size: 8, color: dotColor),
          ),
        ],
      ),
    );
  }
}
