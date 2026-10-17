import 'dart:async';

import 'package:flutter/material.dart';

import '../core/types.dart';
import 'theme.dart';

/// The autorefresh state a page app-bar action reads. [AutoRefreshMixin]
/// implements it, so a page on the mixin can hand itself straight to
/// [PageActions.autoRefresh]; a page that owns its own timer can implement it
/// directly.
abstract interface class AutoRefreshControl {
  /// True while the periodic timer is running.
  bool get autoRefreshActive;

  /// The remembered interval, or null when autorefresh is off.
  Duration? get selectedInterval;

  /// Enables/disables autorefresh; null or [Duration.zero] disables.
  void applyAuto(Duration? interval);
}

/// Autorefresh plumbing shared by the periodic-refresh pages (Docs/App/
/// Connection.md + Service views): owns the timer, remembers the selected
/// interval, and (for shell-tab pages) pauses while the tab is hidden.
/// Pages override [onAutoRefresh] for the tick and [shellTabIndex] when they
/// are a shell tab (route pages leave it null and keep refreshing while open).
mixin AutoRefreshMixin<T extends StatefulWidget> on State<T>
    implements AutoRefreshControl {
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

  @override
  bool get autoRefreshActive => _autoTimer != null;

  @override
  Duration? get selectedInterval => _autoInterval;

  void _onTabChanged() {
    final tab = shellTabIndex;
    if (tab == null) return;
    if (ShellTabs.instance.index == tab) {
      applyAuto(
        _autoInterval,
      ); // tab visible again: restore the remembered interval
    } else {
      _autoTimer?.cancel(); // tab hidden: stop but keep the remembered interval
      _autoTimer = null;
      if (mounted) setState(() {});
    }
  }

  /// Enables/disables autorefresh; null or [Duration.zero] disables.
  @override
  void applyAuto(Duration? interval) {
    _autoTimer?.cancel();
    _autoTimer = null;
    setState(
      () => _autoInterval = (interval == null || interval == Duration.zero)
          ? null
          : interval,
    );
    if (_autoInterval != null) {
      _autoTimer = Timer.periodic(_autoInterval!, (_) => onAutoRefresh());
      onAutoRefreshStarted();
    }
  }
}

/// Name (+ optional index) prompt for the dynamic-block create/edit flows. Dynamic
/// blocks are always the Dynamic type, so no type picker is needed.
/// With `withIndex` the user may pin the new block to an explicit index (filling a
/// None placeholder); an empty index appends. Returns (name, index) or null.
Future<(String, int?)?> promptBlockName(
  BuildContext context, {
  String initialName = '',
  required String title,
  bool withIndex = false,
}) async {
  final nameController = TextEditingController(text: initialName);
  final indexController = TextEditingController();
  final result = await showDialog<(String, int?)>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text(title),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
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
                helperText: 'Fills a deleted (None) slot when given',
              ),
            ),
          ],
        ],
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('Cancel'),
        ),
        FilledButton(
          onPressed: () {
            final name = nameController.text.trim();
            if (name.isEmpty) return;
            final idxText = indexController.text.trim();
            final idx = idxText.isEmpty ? null : int.tryParse(idxText);
            if (idxText.isNotEmpty && idx == null) return;
            Navigator.pop(context, (name, idx));
          },
          child: const Text('OK'),
        ),
      ],
    ),
  );
  nameController.dispose();
  indexController.dispose();
  return result;
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

/// Compact vs expanded shell layout. Phones (narrow) use a navigation Drawer;
/// wider screens keep the always-visible NavigationRail.
class ShellLayout {
  static const double breakpoint = 600;

  static bool isCompact(BuildContext context) =>
      MediaQuery.sizeOf(context).width < breakpoint;
}

/// Key of the shell Scaffold, so the compact-layout app-bar button can open the
/// navigation drawer from any shell tab page.
final GlobalKey<ScaffoldState> shellScaffoldKey = GlobalKey<ScaffoldState>();

/// App-bar leading button shown on the four shell tabs. Renders nothing on wide
/// layouts, where the NavigationRail is always visible.
class ShellDrawerButton extends StatelessWidget {
  const ShellDrawerButton({super.key});

  @override
  Widget build(BuildContext context) {
    if (!ShellLayout.isCompact(context)) return const SizedBox.shrink();
    return IconButton(
      icon: const Icon(Icons.menu),
      tooltip: 'Menu',
      onPressed: () => shellScaffoldKey.currentState?.openDrawer(),
    );
  }
}

/// The bare small orange section label: no padding of its own, so it suits a
/// card body, a dialog section or a picker list. [SectionHeader] is the same
/// label with the page-level inset baked in.
class SectionLabel extends StatelessWidget {
  final String text;

  const SectionLabel(this.text, {super.key});

  @override
  Widget build(BuildContext context) {
    return Text(
      text.toUpperCase(),
      style: Theme.of(
        context,
      ).textTheme.labelSmall?.copyWith(color: kOrange, letterSpacing: 1),
    );
  }
}

/// A small orange section label used by the Settings and info pages. The one
/// section heading once phase 1c lands; its size and spacing come from the
/// theme, not from a literal.
class SectionHeader extends StatelessWidget {
  final String text;

  const SectionHeader(this.text, {super.key});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(
        Spacing.xl,
        Spacing.xl,
        Spacing.xl,
        Spacing.xs,
      ),
      child: SectionLabel(text),
    );
  }
}

/// A titled card holding a list of rows (shared by the Update and Device view pages).
class InfoCard extends StatelessWidget {
  final String title;
  final List<Widget> children;

  const InfoCard({super.key, required this.title, required this.children});

  @override
  Widget build(BuildContext context) {
    return Card(
      color: kSurfaceAlt,
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: Spacing.md),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(
                Spacing.xl,
                Spacing.md,
                Spacing.xl,
                Spacing.xs,
              ),
              child: Text(
                title,
                style: Theme.of(
                  context,
                ).textTheme.titleSmall?.copyWith(color: kOrange),
              ),
            ),
            ...children,
          ],
        ),
      ),
    );
  }
}

/// A label/value row used inside an [InfoCard]. The one label:value row once
/// phase 1c lands.
class InfoRow extends StatelessWidget {
  final String label;
  final String value;

  /// Width of the label column, so a page can align a wider label set.
  final double labelWidth;

  const InfoRow(this.label, this.value, {super.key, this.labelWidth = 140});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(
        horizontal: Spacing.xl,
        vertical: Spacing.hairline,
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
            width: labelWidth,
            child: Text(label, style: Theme.of(context).textTheme.bodySmall),
          ),
          Expanded(child: Text(value)),
        ],
      ),
    );
  }
}

/// A dialog/panel body with a desktop-friendly [maxWidth] that still shrinks to
/// fit a phone screen. The surrounding dialog bounds the actual width, so
/// `double.infinity` resolves to the available space and [maxWidth] caps it.
class DialogBody extends StatelessWidget {
  const DialogBody({
    super.key,
    required this.maxWidth,
    this.height,
    required this.child,
  });

  final double maxWidth;
  final double? height;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    // A fixed body height can exceed a small window and push the dialog's buttons off-screen
    // (the dialog then looks frozen); clamp it to the available height.
    var available = MediaQuery.sizeOf(context).height - 180;
    if (available < 120) available = 120;
    final h = height == null || height! < available ? height : available;
    // Use a FINITE width. `width: double.infinity` has an infinite intrinsic width, which
    // makes AlertDialog's IntrinsicWidth recurse without bound (the dialog freezes on open).
    // A finite SizedBox still shrinks to a narrower parent / dialog.
    return SizedBox(width: maxWidth, height: h, child: child);
  }
}

/// A padded scroll body holding a list of block cards, stretch-fitted to the width.
/// Shared by the Register page's live and backup views.
class BlockScrollBody extends StatelessWidget {
  final List<Widget> children;

  /// A gap inserted between two consecutive children (0 for none).
  final double spacing;

  const BlockScrollBody({
    super.key,
    required this.children,
    this.spacing = Spacing.md,
  });

  @override
  Widget build(BuildContext context) {
    final spaced = <Widget>[];
    for (final (i, child) in children.indexed) {
      if (i > 0 && spacing > 0) spaced.add(SizedBox(height: spacing));
      spaced.add(child);
    }
    return SingleChildScrollView(
      child: Padding(
        padding: Insets.page,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: spaced,
        ),
      ),
    );
  }
}

/// Shows a transient status message (shared by the memory/service pages).
void showSnack(BuildContext context, String message) {
  ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
}

/// Cancel/Confirm alert shared by the destructive actions across pages.
Future<bool> confirmDialog(
  BuildContext context, {
  required String title,
  required String body,
  String confirmLabel = 'Confirm',
}) async {
  return await showDialog<bool>(
        context: context,
        builder: (context) => AlertDialog(
          title: Text(title),
          content: Text(body),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: const Text('Cancel'),
            ),
            FilledButton(
              onPressed: () => Navigator.pop(context, true),
              child: Text(confirmLabel),
            ),
          ],
        ),
      ) ==
      true;
}

/// Rough device-type icons (Docs/App/Devices.md: "Use icons for rough device
/// types").
IconData deviceTypeIcon(DeviceType type) => switch (type) {
  DeviceType.tamuV20A => Icons.developer_board,
  DeviceType.valuV20 => Icons.usb,
  DeviceType.dualAnalogSensor => Icons.sensors,
  DeviceType.unknown => Icons.devices_other,
};

/// Small rounded label chip used across the memory pages.
class ChipLabel extends StatelessWidget {
  const ChipLabel(this.text, {super.key, this.subtle = false});

  final String text;
  final bool subtle;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
      decoration: BoxDecoration(
        color: subtle ? Colors.white.withAlpha(14) : kOrange.withAlpha(46),
        borderRadius: BorderRadius.circular(4),
      ),
      child: Text(
        text,
        style: TextStyle(
          fontSize: 10,
          color: subtle ? Colors.white54 : kOrange,
          fontWeight: subtle ? FontWeight.w400 : FontWeight.w600,
        ),
      ),
    );
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
              child: Row(
                children: [
                  Text(label),
                  const Spacer(),
                  if ((interval ?? Duration.zero) ==
                      (selectedInterval ?? Duration.zero))
                    const Padding(
                      padding: EdgeInsets.only(left: 8),
                      child: Icon(
                        Icons.check,
                        size: 16,
                        color: Colors.greenAccent,
                      ),
                    ),
                ],
              ),
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
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
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

/// Which situation a [StateMessage] is reporting.
enum StateKind { loading, empty, error }

/// The single rendering of a page's loading / empty / error state; the one
/// place those states are drawn once phase 1c lands. Everything reads the
/// theme and the tokens, so a page carries no colour or size of its own.
class StateMessage extends StatelessWidget {
  const StateMessage({
    super.key,
    required this.kind,
    this.message,
    this.details,
    this.icon,
    this.action,
  });

  /// A loading state (a small spinner in place of the icon).
  const StateMessage.loading({
    super.key,
    this.message,
    this.details,
    this.action,
  }) : kind = StateKind.loading,
       icon = null;

  /// An empty state.
  const StateMessage.empty({super.key, this.message, this.details, this.action})
    : kind = StateKind.empty,
      icon = null;

  /// A failure state.
  const StateMessage.error({super.key, this.message, this.details, this.action})
    : kind = StateKind.error,
      icon = null;

  final StateKind kind;

  /// The headline; a kind-specific default is used when null.
  final String? message;

  /// A secondary line, e.g. the underlying error.
  final String? details;

  /// Overrides the kind's default icon.
  final IconData? icon;

  /// An optional action under the message, e.g. a Retry button.
  final Widget? action;

  IconData get _defaultIcon => switch (kind) {
    StateKind.loading => Icons.hourglass_empty,
    StateKind.empty => Icons.inbox_outlined,
    StateKind.error => Icons.error_outline,
  };

  String get _defaultMessage => switch (kind) {
    StateKind.loading => 'Loading...',
    StateKind.empty => 'Nothing here yet',
    StateKind.error => 'Something went wrong',
  };

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final color = switch (kind) {
      StateKind.loading => kTextMuted,
      StateKind.empty => kTextMuted,
      StateKind.error => kError,
    };
    return Center(
      child: Padding(
        padding: Insets.page,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            if (kind == StateKind.loading)
              const SizedBox(
                width: IconSize.lg,
                height: IconSize.lg,
                child: CircularProgressIndicator(strokeWidth: 2),
              )
            else
              Icon(icon ?? _defaultIcon, size: IconSize.xl, color: color),
            const SizedBox(height: Spacing.lg),
            Text(
              message ?? _defaultMessage,
              textAlign: TextAlign.center,
              style: theme.textTheme.titleSmall,
            ),
            if (details != null) ...[
              const SizedBox(height: Spacing.xs),
              Text(
                details!,
                textAlign: TextAlign.center,
                style: theme.textTheme.bodySmall,
              ),
            ],
            if (action != null) ...[
              const SizedBox(height: Spacing.lg),
              action!,
            ],
          ],
        ),
      ),
    );
  }
}

/// The standard page scaffold: a [PageAppBar] plus a body, with the loading /
/// empty / error situation drawn through [StateMessage]. The one page shell
/// once phase 1c lands; a page keeps to it so no scaffold is hand-rolled.
class TamuPage extends StatelessWidget {
  const TamuPage({
    super.key,
    this.title,
    this.titleWidget,
    this.leading,
    this.actions = const [],
    this.auto,
    this.onRefresh,
    this.refreshing = false,
    this.refreshError = false,
    this.appBar,
    this.child,
    this.state,
    this.stateMessage,
    this.stateDetails,
    this.stateAction,
    this.bottomNavigationBar,
    this.floatingActionButton,
  });

  final String? title;
  final Widget? titleWidget;

  /// App-bar leading; the shell tabs pass a [ShellDrawerButton] in compact
  /// layout.
  final Widget? leading;

  /// App-bar actions, in order; build them with [PageActions].
  final List<Widget> actions;

  /// Autorefresh source for the standard refresh action (see [PageAppBar]).
  final AutoRefreshControl? auto;

  /// The refresh tick, paired with [auto].
  final Future<void> Function()? onRefresh;
  final bool refreshing;
  final bool refreshError;

  /// A full app-bar override; when null a [PageAppBar] is built from the
  /// fields above.
  final PreferredSizeWidget? appBar;

  /// The page body, shown when [state] is null.
  final Widget? child;

  /// Loading / empty / error. When set, [child] is ignored and a
  /// [StateMessage] is shown instead.
  final StateKind? state;

  /// Headline for [state].
  final String? stateMessage;

  /// Secondary line for [state].
  final String? stateDetails;

  /// Action for [state].
  final Widget? stateAction;

  final Widget? bottomNavigationBar;

  /// A page-level action button (the Scripts page's "New script").
  final Widget? floatingActionButton;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar:
          appBar ??
          PageAppBar(
            title: title,
            titleWidget: titleWidget,
            leading: leading,
            actions: actions,
            auto: auto,
            onRefresh: onRefresh,
            refreshing: refreshing,
            refreshError: refreshError,
          ),
      body: state != null
          ? StateMessage(
              kind: state!,
              message: stateMessage,
              details: stateDetails,
              action: stateAction,
            )
          : child,
      bottomNavigationBar: bottomNavigationBar,
      floatingActionButton: floatingActionButton,
    );
  }
}

/// The standard page app bar. The title, leading and actions are the caller's;
/// when [auto] and [onRefresh] are both given a refresh / autorefresh action is
/// placed first (the Connection page order). A page that needs the refresh
/// action elsewhere passes its own [PageActions.autoRefresh] in [actions].
class PageAppBar extends StatelessWidget implements PreferredSizeWidget {
  const PageAppBar({
    super.key,
    this.title,
    this.titleWidget,
    this.leading,
    this.actions = const [],
    this.auto,
    this.onRefresh,
    this.refreshing = false,
    this.refreshError = false,
    this.bottom,
  });

  final String? title;
  final Widget? titleWidget;
  final Widget? leading;
  final List<Widget> actions;
  final AutoRefreshControl? auto;
  final Future<void> Function()? onRefresh;
  final bool refreshing;
  final bool refreshError;
  final PreferredSizeWidget? bottom;

  @override
  Size get preferredSize =>
      Size.fromHeight(kToolbarHeight + (bottom?.preferredSize.height ?? 0));

  @override
  Widget build(BuildContext context) {
    return AppBar(
      title: titleWidget ?? (title != null ? Text(title!) : null),
      leading: leading,
      actions: [
        if (auto != null && onRefresh != null)
          PageActions.autoRefresh(
            control: auto!,
            onRefresh: onRefresh!,
            refreshing: refreshing,
            error: refreshError,
          ),
        ...actions,
      ],
      bottom: bottom,
    );
  }
}

/// The standard app-bar action set. Each builder returns a widget for
/// [PageAppBar.actions]; every one reads its look from the theme and the shared
/// components, so an action is never hand-rolled per page.
abstract final class PageActions {
  /// Refresh with the long-press autorefresh menu, bound to an
  /// [AutoRefreshControl] (an [AutoRefreshMixin] page can pass itself).
  static Widget autoRefresh({
    required AutoRefreshControl control,
    required Future<void> Function() onRefresh,
    bool refreshing = false,
    bool error = false,
  }) => refresh(
    onRefresh: onRefresh,
    autoActive: control.autoRefreshActive,
    selectedInterval: control.selectedInterval,
    onSelectAuto: control.applyAuto,
    refreshing: refreshing,
    error: error,
  );

  /// Refresh with explicit autorefresh state, for a page that does not use
  /// [AutoRefreshMixin] (the Connection page owns its timer in the manager).
  static Widget refresh({
    required Future<void> Function() onRefresh,
    required bool autoActive,
    required ValueChanged<Duration?> onSelectAuto,
    Duration? selectedInterval,
    bool refreshing = false,
    bool error = false,
  }) => RefreshButton(
    onRefresh: onRefresh,
    autoActive: autoActive,
    refreshing: refreshing,
    error: error,
    selectedInterval: selectedInterval,
    onSelectAuto: onSelectAuto,
  );

  /// Ask a device to identify itself (blink its LED).
  static Widget identify({
    required Future<void> Function() onIdentify,
    String tooltip = 'Identify device (blink its LED)',
    IconData icon = Icons.visibility_outlined,
  }) => IconButton(tooltip: tooltip, icon: Icon(icon), onPressed: onIdentify);

  /// A leading-icon popup menu: the source picker, sorting, filters all build
  /// on this.
  static Widget menu<T>({
    required IconData icon,
    required T value,
    required List<(T, String)> options,
    required ValueChanged<T> onSelected,
    required String tooltip,
  }) => PopupMenuButton<T>(
    icon: Icon(icon),
    tooltip: tooltip,
    initialValue: value,
    onSelected: onSelected,
    itemBuilder: (_) => [
      for (final (option, label) in options)
        PopupMenuItem(value: option, child: Text(label)),
    ],
  );

  /// The sorting menu.
  static Widget sort<T>({
    required T value,
    required List<(T, String)> options,
    required ValueChanged<T> onSelected,
    String tooltip = 'Sorting',
    IconData icon = Icons.sort,
  }) => menu<T>(
    icon: icon,
    value: value,
    options: options,
    onSelected: onSelected,
    tooltip: tooltip,
  );

  /// The list/graph view switch; the icon reflects the current mode.
  static Widget viewSwitch<T>({
    required T value,
    required List<(T, String, IconData)> options,
    required ValueChanged<T> onSelected,
    String tooltip = 'View',
  }) {
    final (_, _, icon) = options.firstWhere(
      (option) => option.$1 == value,
      orElse: () => options.first,
    );
    return menu<T>(
      icon: icon,
      value: value,
      options: [for (final (option, label, _) in options) (option, label)],
      onSelected: onSelected,
      tooltip: tooltip,
    );
  }
}

/// One device row: type icon, name, `id - type` subtext, a stale marker, an
/// optional badge and trailing widget. The one device row once phase 1c lands;
/// it replaces the per-page rows in Connection / Devices / SNDB / Backup.
class DeviceTile extends StatelessWidget {
  const DeviceTile({
    super.key,
    required this.name,
    this.type,
    this.icon,
    this.idText,
    this.subtitle,
    this.stale = false,
    this.badge,
    this.trailing,
    this.enabled = true,
    this.onTap,
    this.selected = false,
    this.onSelectedChanged,
  });

  /// The device's reported name.
  final String name;

  /// Drives the leading icon and the `id - type` subtext.
  final DeviceType? type;

  /// Overrides the type icon (e.g. Bluetooth vs USB on the Connection page).
  final IconData? icon;

  /// The short id shown before the type in the subtext.
  final String? idText;

  /// A full subtext override, used instead of `idText - type`.
  final String? subtitle;

  /// Marks the device as stale (a dimmed suffix on the name).
  final bool stale;

  /// A static trailing marker, e.g. the core star.
  final Widget? badge;

  /// Interactive trailing content, e.g. the RSSI readout and the star button.
  final Widget? trailing;

  final bool enabled;
  final VoidCallback? onTap;

  /// Selection state for the rows that carry a checkbox (the Backup page).
  final bool selected;
  final ValueChanged<bool>? onSelectedChanged;

  IconData get _icon =>
      icon ?? (type != null ? deviceTypeIcon(type!) : Icons.devices_other);

  String get _subtext => subtitle ?? [?idText, ?type?.label].join(' - ');

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final title = Text.rich(
      TextSpan(
        children: [
          TextSpan(text: name),
          if (stale)
            TextSpan(
              text: '  ·  stale',
              style: theme.textTheme.labelSmall?.copyWith(color: kTextMuted),
            ),
        ],
      ),
      overflow: TextOverflow.ellipsis,
    );
    final sub = _subtext.isEmpty
        ? null
        : Text(
            _subtext,
            style: theme.textTheme.bodySmall,
            overflow: TextOverflow.ellipsis,
          );

    if (onSelectedChanged != null) {
      return CheckboxListTile(
        dense: true,
        secondary: Icon(_icon),
        title: title,
        subtitle: sub,
        value: selected,
        onChanged: enabled
            ? (value) => onSelectedChanged!(value ?? false)
            : null,
      );
    }

    Widget? trail;
    if (badge != null || trailing != null) {
      trail = Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          ?badge,
          if (badge != null && trailing != null)
            const SizedBox(width: Spacing.xs),
          ?trailing,
        ],
      );
    }

    return ListTile(
      enabled: enabled,
      leading: Icon(_icon),
      title: title,
      subtitle: sub,
      trailing: trail,
      onTap: enabled ? onTap : null,
    );
  }
}

/// How a long task ended, for the status line of a [TaskProgress].
enum TaskStatus { running, success, failure }

/// One phase/progress display for update / backup / restore / upload. The one
/// progress display once phase 1c lands.
class TaskProgress extends StatelessWidget {
  const TaskProgress({
    super.key,
    this.label,
    this.value,
    this.completed,
    this.total,
    this.note,
    this.status = TaskStatus.running,
    this.statusText,
  });

  /// The phase line, e.g. `Writing` or `Verifying (pass 2)`.
  final String? label;

  /// Progress in 0..1; null draws an indeterminate bar.
  final double? value;

  /// Step counter shown beside [label] as `completed/total`.
  final int? completed;
  final int? total;

  /// An extra line under the bar, e.g. `12 to correct`.
  final String? note;

  final TaskStatus status;

  /// The line beside the status icon once the task ends.
  final String? statusText;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final counter = (completed == null || total == null)
        ? null
        : '$completed/$total';
    final bar = value?.clamp(0.0, 1.0).toDouble();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        Row(
          children: [
            if (label != null)
              Expanded(child: Text(label!, style: theme.textTheme.bodyMedium))
            else
              const Spacer(),
            if (counter != null)
              Text(counter, style: theme.textTheme.bodySmall),
          ],
        ),
        if (status == TaskStatus.running) ...[
          const SizedBox(height: Spacing.md),
          LinearProgressIndicator(value: bar),
        ],
        if (note != null) ...[
          const SizedBox(height: Spacing.sm),
          Text(note!, style: theme.textTheme.bodySmall),
        ],
        if (status != TaskStatus.running) ...[
          const SizedBox(height: Spacing.md),
          Row(
            children: [
              Icon(
                status == TaskStatus.success ? Icons.check_circle : Icons.error,
                size: IconSize.md,
                color: status == TaskStatus.success ? kSuccess : kError,
              ),
              if (statusText != null) ...[
                const SizedBox(width: Spacing.md),
                Expanded(
                  child: Text(statusText!, style: theme.textTheme.bodyMedium),
                ),
              ],
            ],
          ),
        ],
      ],
    );
  }
}
