/// Subscriptions service UI page.
library;

import 'package:flutter/material.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart' show BlockType, Capability, ProviderSubscription, RequesterSubscription;
import 'subscriptions_dialog.dart' show SubscriptionDialog;

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

class _SubscriptionsPageState extends State<SubscriptionsPage> with SingleTickerProviderStateMixin {
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
          unselectedLabelColor: theme.colorScheme.onPrimary.withValues(alpha: 0.7),
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
              ? Center(child: Text('Error: $_error', style: TextStyle(color: theme.colorScheme.error)))
              : TabBarView(
                  controller: _tabController,
                  children: [
                    _buildProviderTab(),
                    _buildRequesterTab(),
                  ],
                ),
    );
  }

  Widget _buildProviderTab() {
    final db = DeviceDatabase.instance;
    final device = db.byId(widget.deviceId);
    final hasProviderCapability = device?.capabilities != null && (device!.capabilities & Capability.subscriptions) != 0;
    
    if (!hasProviderCapability) {
      return const Center(
        child: Padding(
          padding: EdgeInsets.all(16),
          child: Text(
            'This device does not support acting as a subscription provider.\n'
            'Provider subscriptions require available RAM for the provider table.',
            style: TextStyle(color: Colors.white54),
            textAlign: TextAlign.center,
          ),
        ),
      );
    }
    
    if (_providerSubs.isEmpty) {
      return const Center(child: Text('No provider subscriptions', style: TextStyle(color: Colors.white54)));
    }
    return ListView.builder(
      padding: const EdgeInsets.all(8),
      itemCount: _providerSubs.length,
      itemBuilder: (context, i) => _buildProviderTile(_providerSubs[i]),
    );
  }

  Widget _buildProviderTile(ProviderSubscription sub) {
    return Card(
      margin: const EdgeInsets.symmetric(vertical: 4),
      child: ExpansionTile(
        title: Text('Sub #${sub.index}'),
        subtitle: Text('Requester: ${sub.requesterAddr} → Source: ${_formatBlockInfo(sub.sourceReg)}'),
        children: [
          Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                _detailRow('Trigger', sub.trigger.label),
                _detailRow('Period', '${sub.periodMs} ms'),
                _detailRow('Min Interval', '${sub.minTimeMs} ms'),
                _detailRow('Last Sent', '${sub.lastSentMs} ms ago'),
                _detailRow('Requester Addr', '${sub.requesterAddr}'),
                _detailRow('TRID', '0x${sub.trid.toRadixString(16).padLeft(4, '0')}'),
                _detailRow('Hash', '0x${sub.hash.toRadixString(16).padLeft(8, '0')}'),
              ],
            ),
          ),
        ],
      ),
    );
  }

Widget _buildRequesterTab() {
    final listChildren = <Widget>[
      Expanded(
        child: _requesterSubs.isEmpty
            ? const Center(child: Text('No requester subscriptions', style: TextStyle(color: Colors.white54)))
            : ListView.builder(
                padding: const EdgeInsets.all(8),
                itemCount: _requesterSubs.length,
                itemBuilder: (context, i) => _buildRequesterTile(_requesterSubs[i]),
              ),
      ),
      Padding(
        padding: const EdgeInsets.all(16),
        child: SizedBox(
          width: double.infinity,
          child: FilledButton(
            onPressed: _showAddSubscriptionDialog,
            style: FilledButton.styleFrom(
              padding: const EdgeInsets.symmetric(vertical: 14),
            ),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Icon(Icons.add, size: 20),
                SizedBox(width: 8),
                Text('Add Subscription'),
              ],
            ),
          ),
        ),
      ),
      const SizedBox(height: 8),
    ];
    return Column(children: listChildren);
  }

  Widget _buildRequesterTile(RequesterSubscription sub) {
    return Card(
      margin: const EdgeInsets.symmetric(vertical: 4),
      child: ExpansionTile(
        title: Text('Sub #${sub.index} (TRID: 0x${sub.trid.toRadixString(16).padLeft(4, '0')})'),
        subtitle: Text('Target: ${_formatBlockInfo(sub.targetReg)} ← Source: ${_formatBlockInfo(sub.sourceReg)}'),
        trailing: IconButton(
          icon: const Icon(Icons.delete, color: Colors.red),
          onPressed: () => _deleteRequesterSubscription(sub.index),
        ),
        children: [
          Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                _detailRow('Provider', '${sub.providerAddr}'),
                _detailRow('Trigger', sub.trigger.label),
                _detailRow('Period', '${sub.periodMs} ms'),
                _detailRow('Min Interval', '${sub.minTimeMs} ms'),
                const SizedBox(height: 8),
                FilledButton(
                  onPressed: () => _showEditSubscriptionDialog(sub),
                  child: const Text('Edit'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _detailRow(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(width: 120, child: Text(label, style: const TextStyle(color: Colors.white54))),
          Expanded(child: Text(value, style: const TextStyle(fontFamily: 'monospace'))),
        ],
      ),
    );
  }

  String _formatBlockInfo(int bi) {
    final type = (bi >> 22) & 0x3FF;
    final inst = (bi >> 16) & 0x3F;
    final field = (bi >> 8) & 0xFF;
    final key = bi & 0xFF;
    final typeLabel = BlockType.fromValue(type).label;
    return '$typeLabel[$inst].f$field.k$key';
  }

  void _showAddSubscriptionDialog() {
    showDialog(
      context: context,
      builder: (context) => SubscriptionDialog(
        client: _client,
        regClient: _regClient,
        currentSubs: _requesterSubs,
        onSaved: _loadSubscriptions,
      ),
    );
  }

  void _showEditSubscriptionDialog(RequesterSubscription sub) {
    showDialog(
      context: context,
      builder: (context) => SubscriptionDialog(
        client: _client,
        regClient: _regClient,
        currentSubs: _requesterSubs,
        existing: sub,
        onSaved: _loadSubscriptions,
      ),
    );
  }

  Future<void> _deleteRequesterSubscription(int index) async {
    final ok = await _client.setRequesterSubscription(index, entry: null);
    if (ok) {
      await _loadSubscriptions();
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Subscription deleted')));
      }
    } else {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Failed to delete subscription')));
      }
    }
  }
}
