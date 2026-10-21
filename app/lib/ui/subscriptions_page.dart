/// Subscriptions service UI page.
library;

import 'package:flutter/material.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart'
    show
        Capability,
        ProviderSubscription,
        RequesterSubscription,
        blockInfoLabel;
import 'subscriptions_dialog.dart' show SubscriptionDialog;
import 'theme.dart';
import 'viewer_kit.dart';
import 'widgets.dart' show showSnack;

class SubscriptionsPage extends StatefulWidget {
  final int deviceId;
  final String deviceName;

  const SubscriptionsPage({
    super.key,
    required this.deviceId,
    required this.deviceName,
  });

  @override
  State<SubscriptionsPage> createState() => _SubscriptionsPageState();
}

class _SubscriptionsPageState extends State<SubscriptionsPage>
    with SingleTickerProviderStateMixin {
  late final TabController _tabController;
  late final SubscriptionClient _client;
  late final RegisterClient _regClient;

  List<ProviderSubscription> _providerSubs = [];
  List<RequesterSubscription> _requesterSubs = [];
  bool _loading = true;
  String? _error;

  @override
  void initState() {
    super.initState();
    _tabController = TabController(length: 2, vsync: this);
    _client = SubscriptionClient(deviceId: widget.deviceId);
    _regClient = RegisterClient(deviceId: widget.deviceId);
    _client.startListening();
    _loadSubscriptions();
  }

  @override
  void dispose() {
    _client.stopListening();
    _tabController.dispose();
    _client.dispose();
    super.dispose();
  }

  Future<void> _loadSubscriptions() async {
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final provider = await _client.getProviderSubscriptions();
      final requester = await _client.getRequesterSubscriptions();
      setState(() {
        _providerSubs = provider;
        _requesterSubs = requester;
        _loading = false;
      });
    } catch (e) {
      setState(() {
        _error = e.toString();
        _loading = false;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      appBar: AppBar(
        title: Text('Subscriptions - ${widget.deviceName}'),
        bottom: TabBar(
          controller: _tabController,
          labelColor: theme.colorScheme.onPrimary,
          unselectedLabelColor: theme.colorScheme.onPrimary.withValues(
            alpha: 0.7,
          ),
          indicatorColor: theme.colorScheme.onPrimary,
          tabs: const [
            Tab(text: 'Provider (incoming)'),
            Tab(text: 'Requester (outgoing)'),
          ],
        ),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            onPressed: _loadSubscriptions,
            tooltip: 'Refresh',
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _error != null
          ? Center(
              child: Text(
                'Error: $_error',
                style: theme.textTheme.bodySmall?.copyWith(color: kError),
              ),
            )
          : TabBarView(
              controller: _tabController,
              children: [_buildProviderTab(), _buildRequesterTab()],
            ),
    );
  }

  Widget _buildProviderTab() {
    final db = DeviceDatabase.instance;
    final device = db.byId(widget.deviceId);
    final hasProviderCapability =
        device?.capabilities != null &&
        (device!.capabilities & Capability.subscriptionProvide) != 0;

    if (!hasProviderCapability) {
      return _unavailable(
        'This device does not support acting as a subscription provider.\n'
        'Provider subscriptions require available RAM for the provider table.',
      );
    }

    return TamuPagedList<ProviderSubscription>(
      items: _providerSubs,
      padding: const EdgeInsets.all(Spacing.md),
      emptyMessage: 'No provider subscriptions',
      itemBuilder: (context, sub, _) => _buildProviderTile(sub),
    );
  }

  Widget _buildProviderTile(ProviderSubscription sub) {
    return Card(
      margin: const EdgeInsets.symmetric(vertical: Spacing.xs),
      child: ExpansionTile(
        title: Text('Sub #${sub.index}'),
        subtitle: Text(
          'Requester: ${sub.requesterAddr} → Source: ${_formatBlockInfo(sub.sourceReg)}',
        ),
        children: [
          Padding(
            padding: Insets.cardBody,
            child: _details([
              ('Trigger', sub.trigger.label),
              ('Period', '${sub.periodMs} ms'),
              ('Min Interval', '${sub.minTimeMs} ms'),
              ('Deadzone', '${sub.deadzone}'),
              // lastSentMs is an absolute ms value in the provider's local uptime, not a
              // "time ago" delta (Docs/Services/Subscriptions.md:43).
              ('Last Sent', '${sub.lastSentMs} ms (uptime)'),
              ('Requester Addr', '${sub.requesterAddr}'),
              ('TRID', '0x${sub.trid.toRadixString(16).padLeft(4, '0')}'),
              ('Hash', '0x${sub.hash.toRadixString(16).padLeft(8, '0')}'),
            ]),
          ),
        ],
      ),
    );
  }

  Widget _buildRequesterTab() {
    // The requester side is offered when the device advertises the Subscriptions Request
    // capability (a requester need not be a core).
    final device = DeviceDatabase.instance.byId(widget.deviceId);
    final canRequest =
        device != null &&
        (device.capabilities & Capability.subscriptionRequest) != 0;
    if (!canRequest) {
      return _unavailable(
        'This device does not support requester subscriptions.\n'
        'Requester subscriptions are a core feature.',
      );
    }
    return Column(
      children: [
        Expanded(
          child: TamuPagedList<RequesterSubscription>(
            items: _requesterSubs,
            padding: const EdgeInsets.all(Spacing.md),
            emptyMessage: 'No requester subscriptions',
            itemBuilder: (context, sub, _) => _buildRequesterTile(sub),
          ),
        ),
        Padding(
          padding: Insets.cardBody,
          child: SizedBox(
            width: double.infinity,
            child: FilledButton(
              onPressed: () => _showSubscriptionDialog(),
              child: const Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(Icons.add, size: IconSize.lg),
                  SizedBox(width: Spacing.md),
                  Text('Add Subscription'),
                ],
              ),
            ),
          ),
        ),
        const SizedBox(height: Spacing.md),
      ],
    );
  }

  Widget _buildRequesterTile(RequesterSubscription sub) {
    return Card(
      margin: const EdgeInsets.symmetric(vertical: Spacing.xs),
      child: ExpansionTile(
        title: Text(
          'Sub #${sub.index} (TRID: 0x${sub.trid.toRadixString(16).padLeft(4, '0')})',
        ),
        subtitle: Text(
          'Target: ${_formatBlockInfo(sub.targetReg)} ← Source: ${_formatBlockInfo(sub.sourceReg)}',
        ),
        trailing: IconButton(
          icon: const Icon(Icons.delete, size: IconSize.lg, color: kError),
          onPressed: () => _deleteRequesterSubscription(sub.index),
        ),
        children: [
          Padding(
            padding: Insets.cardBody,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                _details([
                  ('Provider', '${sub.providerAddr}'),
                  ('Trigger', sub.trigger.label),
                  ('Period', '${sub.periodMs} ms'),
                  ('Min Interval', '${sub.minTimeMs} ms'),
                  ('Deadzone', '${sub.deadzone}'),
                ]),
                const SizedBox(height: Spacing.md),
                FilledButton(
                  onPressed: () => _showSubscriptionDialog(sub),
                  child: const Text('Edit'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  /// A tab's "this device cannot do that" state.
  Widget _unavailable(String message) => Center(
    child: Padding(
      padding: const EdgeInsets.all(Spacing.xl),
      child: Text(
        message,
        textAlign: TextAlign.center,
        style: Theme.of(
          context,
        ).textTheme.bodySmall?.copyWith(color: kTextMuted),
      ),
    ),
  );

  /// One subscription's detail fields: the toolkit's table with a muted label
  /// column and a monospace value column, so a tile never hand-places a label width.
  Widget _details(List<(String, String)> fields) => TamuTable<(String, String)>(
    embedded: true,
    showHeader: false,
    columns: [
      TamuColumn<(String, String)>(
        label: 'Field',
        text: (r) => r.$1,
        muted: true,
        flex: 2,
      ),
      TamuColumn<(String, String)>(
        label: 'Value',
        text: (r) => r.$2,
        mono: true,
        flex: 3,
      ),
    ],
    rows: fields,
  );

  String _formatBlockInfo(int bi) => blockInfoLabel(bi);

  /// Opens the create/edit dialog: `existing` selects edit mode, null creates.
  void _showSubscriptionDialog([RequesterSubscription? existing]) {
    showDialog(
      context: context,
      builder: (context) => SubscriptionDialog(
        client: _client,
        regClient: _regClient,
        currentSubs: _requesterSubs,
        existing: existing,
        onSaved: _loadSubscriptions,
      ),
    );
  }

  Future<void> _deleteRequesterSubscription(int index) async {
    // NOTE: setRequesterSubscription() treats a non-null reply as success, but a FAIL reply
    // also carries a payload (Issues.md "App (subscriptions)"); surfacing failure needs the
    // core request() to expose the response flags. Until then a provider-only DAS can
    // report a delete as successful.
    final ok = await _client.setRequesterSubscription(index, entry: null);
    if (ok) {
      await _loadSubscriptions();
      if (mounted) {
        showSnack(context, 'Subscription deleted');
      }
    } else {
      if (mounted) showSnack(context, 'Failed to delete subscription');
    }
  }
}
