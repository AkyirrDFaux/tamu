import 'dart:async';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/types.dart';
import 'device_view_page.dart';
import 'theme.dart';
import 'widgets.dart';

/// Declared size of the core star in a device row: a marker rather than text, so
/// it keeps this slot whatever the text scale. The gap around it belongs to
/// [DeviceTile], which owns the row's spacing.
const double _coreStarSize = 18;

/// Devices page (Docs/App/Devices.md): main interaction layer with list and
/// graph views.
class DevicesPage extends StatefulWidget {
  const DevicesPage({super.key});

  @override
  State<DevicesPage> createState() => _DevicesPageState();
}

enum _ViewMode { list, graph }

class _DevicesPageState extends State<DevicesPage>
    with AutoRefreshMixin<DevicesPage> {
  final _db = DeviceDatabase.instance;
  _ViewMode _mode = _ViewMode.list;
  bool _refreshing = false;

  @override
  int? get shellTabIndex => 1; // Devices tab

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();

  @override
  void onAutoRefreshStarted() {
    if (!_db.isRefreshing) _refresh();
  }

  // Filters (Docs/App/Devices.md bottom of screen).
  int? _netFilter;
  DeviceType? _typeFilter;
  DevicesSort _sort = DevicesSort.id;

  List<DeviceEntry> get _filtered {
    var devices = _db.all.where((d) {
      if (_netFilter != null && d.net != _netFilter) return false;
      if (_typeFilter != null && d.type != _typeFilter) return false;
      return true;
    }).toList();
    switch (_sort) {
      case DevicesSort.id:
        devices.sort((a, b) => a.id.compareTo(b.id));
      case DevicesSort.name:
        devices.sort(
          (a, b) => a.displayName.toLowerCase().compareTo(
            b.displayName.toLowerCase(),
          ),
        );
      case DevicesSort.deviceType:
        devices.sort((a, b) => a.type.value.compareTo(b.type.value));
    }
    return devices;
  }

  Future<void> _refresh() async {
    if (_refreshing || !ConnectionManager.instance.isConnected) return;
    setState(() => _refreshing = true);
    try {
      await _db.refreshNetwork();
    } finally {
      if (mounted) setState(() => _refreshing = false);
    }
  }

  Widget _viewMenu() {
    return PopupMenuButton<_ViewMode>(
      icon: Icon(
        _mode == _ViewMode.list ? Icons.view_list : Icons.account_tree,
      ),
      tooltip: 'View',
      initialValue: _mode,
      onSelected: (mode) => setState(() => _mode = mode),
      itemBuilder: (_) => const [
        PopupMenuItem(value: _ViewMode.list, child: Text('List view')),
        PopupMenuItem(value: _ViewMode.graph, child: Text('Graph view')),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    final compact = ShellLayout.isCompact(context);
    return ListenableBuilder(
      listenable: _db,
      builder: (context, _) => Scaffold(
        appBar: AppBar(
          title: const Text('Devices'),
          leading: compact ? const ShellDrawerButton() : _viewMenu(),
          actions: [
            if (compact) _viewMenu(),
            RefreshButton(
              onRefresh: _refresh,
              autoActive: autoRefreshActive,
              refreshing: _db.isRefreshing || _refreshing,
              error: _db.lastError != null,
              selectedInterval: selectedInterval,
              onSelectAuto: applyAuto,
            ),
          ],
        ),
        body: Column(
          children: [
            Expanded(child: _buildBody()),
            const Divider(height: 1),
            _buildFilterBar(),
          ],
        ),
      ),
    );
  }

  Widget _buildBody() {
    if (!ConnectionManager.instance.isConnected) {
      return const Center(child: Text('Not connected'));
    }
    final devices = _filtered;
    if (devices.isEmpty) {
      return Center(child: Text(_db.lastError ?? 'No devices discovered yet'));
    }
    return AnimatedSwitcher(
      duration: const Duration(milliseconds: 150),
      child: _mode == _ViewMode.list
          ? ListView.builder(
              key: const ValueKey('list'),
              itemCount: devices.length,
              itemBuilder: (context, index) =>
                  _deviceTile(context, devices[index]),
            )
          : GraphView(key: const ValueKey('graph'), devices: devices),
    );
  }

  Widget _deviceTile(BuildContext context, DeviceEntry device) {
    return DeviceTile(
      type: device.type,
      name: device.displayName,
      stale: device.stale,
      idText: idToString(device.id),
      badge: device.isCore
          ? const Icon(Icons.star, color: kOrange, size: _coreStarSize)
          : null,
      onTap: () => Navigator.of(context).push(
        MaterialPageRoute(builder: (_) => DeviceViewPage(deviceId: device.id)),
      ),
    );
  }

  Widget _buildFilterBar() {
    return DevicesFilterBar(
      netFilter: _netFilter,
      typeFilter: _typeFilter,
      sort: _sort,
      onCycleNet: _cycleNet,
      onCycleType: _cycleType,
      onSortChanged: (sort) => setState(() => _sort = sort),
    );
  }

  void _cycleNet() {
    final nets = _db.all.map((d) => d.net).toSet().toList()..sort();
    setState(() {
      if (_netFilter == null) {
        _netFilter = nets.isNotEmpty ? nets.first : null;
      } else {
        final index = nets.indexOf(_netFilter!);
        _netFilter = index >= 0 && index < nets.length - 1
            ? nets[index + 1]
            : null;
      }
    });
  }

  void _cycleType() {
    final types = _db.all.map((d) => d.type).toSet().toList();
    setState(() {
      if (_typeFilter == null) {
        _typeFilter = types.isNotEmpty ? types.first : null;
      } else {
        final index = types.indexWhere((t) => t.value == _typeFilter!.value);
        _typeFilter = index >= 0 && index < types.length - 1
            ? types[index + 1]
            : null;
      }
    });
  }
}

/// The filter bar at the bottom of the Devices page (Docs/App/Devices.md): the
/// net chip, the type chip and the sort menu, in that order.
///
/// The bar is a [Wrap], not a fixed-height `Row`: measured with its longest real
/// labels ("Net 255", "Type: Tamu v2.0A") the three controls want 140 + 176 + 48
/// px plus two [Spacing.md] gaps = 380 px, so a row overflowed a 360 px wide
/// screen. As a wrap the same three controls stay on one line when the width
/// allows it and move to the next line when it does not, and the bar's height
/// follows its content instead of being pinned at 56 px.
class DevicesFilterBar extends StatelessWidget {
  const DevicesFilterBar({
    super.key,
    required this.netFilter,
    required this.typeFilter,
    required this.sort,
    required this.onCycleNet,
    required this.onCycleType,
    required this.onSortChanged,
  });

  /// The selected net, or null for "all".
  final int? netFilter;

  /// The selected device type, or null for "all".
  final DeviceType? typeFilter;

  final DevicesSort sort;

  /// Tapping a chip cycles its value (null -> first -> ... -> null).
  final VoidCallback onCycleNet;
  final VoidCallback onCycleType;
  final ValueChanged<DevicesSort> onSortChanged;

  @override
  Widget build(BuildContext context) {
    return Wrap(
      spacing: Spacing.md,
      runSpacing: Spacing.xs,
      crossAxisAlignment: WrapCrossAlignment.center,
      children: [
        FilterChip(
          label: Text(netFilter == null ? 'Net: all' : 'Net $netFilter'),
          selected: netFilter != null,
          onSelected: (_) => onCycleNet(),
        ),
        FilterChip(
          label: Text(typeFilter?.label ?? 'Type: all'),
          selected: typeFilter != null,
          onSelected: (_) => onCycleType(),
        ),
        PopupMenuButton<DevicesSort>(
          tooltip: 'Sorting',
          initialValue: sort,
          onSelected: onSortChanged,
          itemBuilder: (_) => const [
            PopupMenuItem(value: DevicesSort.id, child: Text('Sort: ID')),
            PopupMenuItem(value: DevicesSort.name, child: Text('Sort: Name')),
            PopupMenuItem(
              value: DevicesSort.deviceType,
              child: Text('Sort: Type'),
            ),
          ],
        ),
      ],
    );
  }
}

/// The order the list shows devices in (Docs/App/Devices.md: Sorting, cycled by
/// the sort menu).
enum DevicesSort { id, name, deviceType }

/// Pannable tree view (Docs/App/Devices.md): cores topmost, then routers
/// (none implemented yet), then nodes stacked below them.
///
/// The canvas is the width the view is given, and the block size is derived from
/// it instead of being a constant: rows of three are the *wide* arrangement (the
/// docs' "rows of three", unresolved in the spec), so as the width shrinks the
/// rows hold two blocks and then one - the block never gets narrower than
/// [_minBlockWidth] or wider than [_maxBlockWidth]. The canvas stays pannable and
/// zoomable, so the rest of a tall graph is reached by dragging it.
class GraphView extends StatelessWidget {
  final List<DeviceEntry> devices;

  const GraphView({super.key, required this.devices});

  /// The narrowest a block may get: [Spacing.lg] of padding either side, then
  /// room for the icon and a few glyphs of the name. Checked by [_blocksPerRow]:
  /// below the width that fits fewer blocks, the row holds fewer instead of
  /// shrinking the block further.
  static const double _minBlockWidth = 128;

  /// The widest a block grows. Past this the extra width stays as margin, so a
  /// wide window does not stretch three blocks across the whole canvas.
  static const double _maxBlockWidth = 200;

  /// The widest arrangement: the docs' rows of three.
  static const int _maxPerRow = 3;

  /// Gap between two blocks in a row (and between rows).
  static const double _gap = Spacing.lg;

  @override
  Widget build(BuildContext context) {
    final cores = devices.where((d) => d.isCore).toList();
    final nodes = devices.where((d) => !d.isCore).toList();

    return LayoutBuilder(
      builder: (context, constraints) {
        // The space the canvas has to work with: the view's own width, less the
        // page padding the blocks sit inside.
        final canvas = constraints.maxWidth;
        final inner = canvas - Spacing.xxl * 2;
        final perRow = _blocksPerRow(inner);
        final blockWidth = _blockWidth(inner, perRow);

        return InteractiveViewer(
          constrained: false,
          maxScale: 4,
          child: SizedBox(
            width: canvas,
            child: Padding(
              padding: const EdgeInsets.all(Spacing.xxl),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.center,
                children: [
                  if (cores.isEmpty)
                    const Text('No core discovered')
                  else ...[
                    for (final core in cores)
                      _deviceBlock(context, core, blockWidth),
                  ],
                  if (nodes.isNotEmpty) ...[
                    const SizedBox(height: Spacing.xxl),
                    // A [Wrap] rather than hand-cut rows: it fits as many blocks
                    // per line as the width allows (never more than [perRow], so
                    // three at most), and a rounding hair cannot overflow it.
                    Wrap(
                      alignment: WrapAlignment.center,
                      spacing: _gap,
                      runSpacing: _gap,
                      children: [
                        for (final node in nodes)
                          _deviceBlock(context, node, blockWidth),
                      ],
                    ),
                  ],
                ],
              ),
            ),
          ),
        );
      },
    );
  }

  /// How many blocks fit on one line of [inner] width, capped at [_maxPerRow]:
  /// three in the wide arrangement, fewer as the width shrinks, never zero.
  static int _blocksPerRow(double inner) {
    final fit = ((inner + _gap) / (_minBlockWidth + _gap)).floor();
    return fit.clamp(1, _maxPerRow);
  }

  /// The even share of [inner] across [perRow] blocks, between the declared
  /// minimum and maximum. A view narrower than the minimum still shrinks the
  /// block to what it has (the minimum is a floor, not a promise to overflow).
  static double _blockWidth(double inner, int perRow) {
    final even = (inner - _gap * (perRow - 1)) / perRow;
    final clamped = even.clamp(_minBlockWidth, _maxBlockWidth);
    return clamped < inner ? clamped : inner;
  }

  Widget _deviceBlock(BuildContext context, DeviceEntry device, double width) {
    return Card(
      color: kSurfaceAlt,
      // The gaps are the row's, not the card's: [Wrap] spaces the blocks and the
      // derived width is the whole slot a block occupies.
      margin: EdgeInsets.zero,
      child: InkWell(
        borderRadius: BorderRadius.circular(Radii.lg),
        onTap: () => Navigator.of(context).push(
          MaterialPageRoute(
            builder: (_) => DeviceViewPage(deviceId: device.id),
          ),
        ),
        child: SizedBox(
          width: width,
          child: Padding(
            padding: const EdgeInsets.all(Spacing.lg),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(deviceTypeIcon(device.type), color: kOrange),
                const SizedBox(height: Spacing.sm),
                Text(
                  device.displayName,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
                Text(
                  '${idToString(device.id)} - ${device.type.label}',
                  style: Theme.of(context).textTheme.bodySmall,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
