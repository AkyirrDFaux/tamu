import 'package:flutter/material.dart';

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
              onPressed: () => Navigator.pop(context, interval),
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
    if (!context.mounted || selected == null && selectedInterval == null) {
      return;
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
