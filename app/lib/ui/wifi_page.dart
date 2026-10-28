import 'dart:async';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/register_client.dart';
import '../core/transport.dart' show UsbTransport;
import '../core/types.dart';
import '../core/wifi_settings.dart';
import 'formatters.dart' show kSecretMask;
import 'theme.dart';
import 'widgets.dart';

/// The WiFi panel: the `WiFi Settings` block (BlockType `0x0C`) read and written through the
/// register path, with the state the device reports and nothing invented
/// (`core/wifi_settings.dart` holds the field map and the condition rules).
///
/// The block is a **compile-in firmware option that is off by default**, so the panel is
/// optional as a whole: it is offered only when the block came back in the device's own block
/// enumeration ([hasWifiSettingsBlock]), and when it is not there this page renders nothing at
/// all - no empty card, no "unavailable" message. [WifiPage] is only reached from a device view
/// that made the same check, so the blank case is the block disappearing under an open page.
class WifiPage extends StatefulWidget {
  final int deviceId;

  /// Test seam: the refresh paths only run on a live link, which a widget test cannot fake.
  final bool Function()? isConnected;

  /// Test seam: whether this app's own link to the device is USB. The block refuses a write
  /// that arrived over BLE (firmware `WifiSettingsGlue.h`), so the panel says so up front
  /// instead of letting the user fill a form the device will reject.
  final bool Function()? isUsbLink;

  /// Test seam: how the page builds its register client. Defaults to a real [RegisterClient].
  final RegisterClient Function(int deviceId)? clientFactory;

  const WifiPage({
    super.key,
    required this.deviceId,
    this.isConnected,
    this.isUsbLink,
    this.clientFactory,
  });

  @override
  State<WifiPage> createState() => _WifiPageState();
}

class _WifiPageState extends State<WifiPage> with AutoRefreshMixin<WifiPage> {
  late final RegisterClient _client =
      (widget.clientFactory ?? (int id) => RegisterClient(deviceId: id))(
        widget.deviceId,
      );

  /// The last reading of the block, or null before the first successful one.
  WifiSnapshot? _snapshot;

  /// The raw field reads behind [_snapshot]: the write path takes a field's declared type and
  /// width from here instead of guessing them.
  Map<int, ({ValueInfo meta, List<int> value})?> _reads = const {};

  /// False when the device's enumeration does not carry the block: the page then draws nothing.
  bool _present = true;

  bool _refreshing = false;
  bool _busy = false;
  bool _revealPassword = false;
  String? _error;

  bool get _connected =>
      (widget.isConnected ?? (() => ConnectionManager.instance.isConnected))();

  /// True when this app talks to the device over USB, which is the only link the block accepts
  /// a write on.
  bool get _overUsb =>
      (widget.isUsbLink ??
      () => ConnectionManager.instance.transport is UsbTransport)();

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();

  void _snack(String message) {
    if (!mounted) return;
    showSnack(context, message);
  }

  Future<void> _refresh() async {
    if (!_connected || _refreshing || _busy) return;
    setState(() => _refreshing = true);
    try {
      final types = await _client.enumerateBlockTypes();
      if (!mounted) return;
      if (types == null) {
        setState(() {
          _refreshing = false;
          _error = 'The device did not answer the block enumeration';
        });
        return;
      }
      if (!hasWifiSettingsBlock(types)) {
        // No block, no panel. This is the stock firmware's answer, so it is not a failure.
        setState(() {
          _refreshing = false;
          _present = false;
          _error = null;
          _snapshot = null;
        });
        return;
      }

      final reads = <int, ({ValueInfo meta, List<int> value})?>{};
      for (var field = 0; field < wifiFieldCount; field++) {
        reads[field] = await _client.readBlockField(
          wifiSettingsBlockTypeValue,
          0,
          field,
          0,
        );
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
            : 'The WiFi block is present but did not answer a read';
        _snapshot = answered ? WifiSnapshot.fromFields(reads) : null;
      });
    } finally {
      if (mounted && _refreshing) setState(() => _refreshing = false);
    }
  }

  /// The write's declared type: the device's own for a field it just answered for, and the
  /// block's documented type for one it did not (Enabled is a Bool, the rest are Text).
  int _writeType(int field) =>
      _reads[field]?.meta.type ??
      (field == wifiFieldEnabled ? DataType.bool_.value : DataType.text.value);

  /// A text value at the width the device declared. The firmware pads a SHORTER Text write
  /// itself, but refuses to pad a field wider than its 32-byte staging buffer, so the app sends
  /// the full width: both rules then hold whatever the field's size is.
  List<int> _wireText(int field, String text) {
    final bytes = text.codeUnits.toList();
    final size = _reads[field]?.meta.size ?? 0;
    if (size > 0) {
      if (bytes.length > size) {
        bytes.length = size;
      } else {
        bytes.addAll(List<int>.filled(size - bytes.length, 0));
      }
    }
    return bytes;
  }

  /// Writes one field and then persists it where the device mirrors it: Enabled and SSID are
  /// Persistent fields, which the device copies into `.SV` on a Save All (the same button the
  /// register view uses). The password lives in NVS and is durable as soon as its trigger has
  /// run.
  Future<void> _write(int field, List<int> bytes) async {
    if (_busy) return;
    setState(() => _busy = true);
    final reply = await _client.writeBlockField(
      wifiSettingsBlockTypeValue,
      0,
      field,
      0,
      ValueInfo(
        type: _writeType(field),
        size: bytes.length,
        // The field's own flags where the device just reported them (Enabled is Trigger +
        // Persistent, SSID Persistent), so the write describes the field the way the device's
        // table does.
        flags: _reads[field]?.meta.flags ?? ValueFlags.persistent,
      ),
      bytes,
    );
    final wrote = reply != null;
    // Only a PERSISTENT field is mirrored into `.SV` by a Save All; the password lives in NVS
    // and is durable as soon as its trigger has run, so a save after it would report a step that
    // does nothing for this field.
    final persists = field != wifiFieldPassword;
    final saved = wrote && persists && await _client.saveAll();
    if (!mounted) return;
    // A typed secret is dropped from the screen the moment it is written; whether the app may
    // show it again is decided by the next read, not by what was typed here.
    setState(() {
      _busy = false;
      _revealPassword = false;
    });
    _snack(
      !wrote
          ? (_overUsb
                ? 'The device refused the write'
                : 'The device refused the write - WiFi settings can only be changed over USB')
          : !persists
          ? 'Password stored on the device'
          : saved
          ? 'Saved to the device'
          : 'Written, but the device did not save it',
    );
    await _refresh();
  }

  Future<void> _setEnabled(bool enabled) =>
      _write(wifiFieldEnabled, [enabled ? 1 : 0]);

  Future<void> _editSsid(String current) async {
    final text = await _promptText(
      title: 'SSID',
      initial: current,
      maxChars: 32,
    );
    if (text == null) return;
    await _write(wifiFieldSsid, _wireText(wifiFieldSsid, text));
  }

  /// The password editor never starts from the device's value: a secret is typed fresh, and the
  /// app only keeps what the DEVICE reads back.
  Future<void> _editPassword() async {
    final text = await _promptText(
      title: 'Password',
      initial: '',
      maxChars: 63,
      secret: true,
    );
    if (text == null) return;
    await _write(wifiFieldPassword, _wireText(wifiFieldPassword, text));
  }

  Future<String?> _promptText({
    required String title,
    required String initial,
    required int maxChars,
    bool secret = false,
  }) {
    final controller = TextEditingController(text: initial);
    return showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(title),
        content: TextField(
          controller: controller,
          autofocus: true,
          maxLength: maxChars,
          obscureText: secret,
          autocorrect: false,
          enableSuggestions: !secret,
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Cancel'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(context, controller.text),
            child: const Text('OK'),
          ),
        ],
      ),
    );
  }

  /// What the password row prints. A device that answers the read with a value reads the
  /// password back, so the app may show it masked with a deliberate reveal; an empty answer
  /// means the device keeps it secret, and the app must never show what it once wrote.
  String _passwordText(WifiSnapshot snapshot) {
    if (!snapshot.passwordReadsBack) return 'not read back by the device';
    return _revealPassword ? snapshot.password : kSecretMask;
  }

  @override
  Widget build(BuildContext context) {
    final actions = [
      PageActions.autoRefresh(
        control: this,
        onRefresh: _refresh,
        refreshing: _refreshing,
        error: _error != null,
      ),
    ];

    // The block is missing: draw the panel as nothing at all (not an empty card, not an error).
    if (!_present) return TamuPage(title: 'WiFi', actions: actions);

    final snapshot = _snapshot;
    if (_error != null && snapshot == null) {
      return TamuPage(
        title: 'WiFi',
        actions: actions,
        state: StateKind.error,
        stateMessage: 'WiFi block unavailable',
        stateDetails: _error,
        stateAction: OutlinedButton(
          onPressed: _refresh,
          child: const Text('Retry'),
        ),
      );
    }
    if (snapshot == null) {
      return TamuPage(
        title: 'WiFi',
        actions: actions,
        state: StateKind.loading,
        stateMessage: 'Reading WiFi settings',
      );
    }

    return TamuPage(
      title: 'WiFi',
      actions: actions,
      child: ListView(
        padding: Insets.page,
        children: [
          _WifiConditionBanner(snapshot: snapshot),
          if (!_overUsb) ...[
            const SizedBox(height: Spacing.md),
            // The device refuses a write that arrived over BLE (firmware WifiSettingsGlue.h), so
            // say it before the form is filled rather than after a rejected write.
            Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Icon(Icons.usb, size: IconSize.sm, color: kWarning),
                const SizedBox(width: Spacing.sm),
                Expanded(
                  child: Text(
                    'Changing these settings needs a USB link to the device; over any other '
                    'link the device refuses the write.',
                    style: Theme.of(context).textTheme.bodySmall,
                  ),
                ),
              ],
            ),
          ],
          const SizedBox(height: Spacing.lg),
          InfoCard(
            title: 'WiFi Settings',
            children: [
              SwitchListTile(
                dense: true,
                contentPadding: const EdgeInsets.symmetric(
                  horizontal: Spacing.xl,
                ),
                title: const Text('Enabled'),
                subtitle: Text(
                  snapshot.enabled ? 'The radio is used' : 'The radio is off',
                ),
                value: snapshot.enabled,
                onChanged: _busy ? null : _setEnabled,
              ),
              ListTile(
                dense: true,
                contentPadding: const EdgeInsets.symmetric(
                  horizontal: Spacing.xl,
                ),
                title: const Text('SSID'),
                subtitle: Text(
                  snapshot.ssid.isEmpty ? 'not set' : snapshot.ssid,
                ),
                trailing: IconButton(
                  tooltip: 'Set the SSID',
                  icon: const Icon(Icons.edit_outlined, size: IconSize.sm),
                  onPressed: _busy ? null : () => _editSsid(snapshot.ssid),
                ),
              ),
              ListTile(
                dense: true,
                contentPadding: const EdgeInsets.symmetric(
                  horizontal: Spacing.xl,
                ),
                title: const Text('Password'),
                subtitle: Text(_passwordText(snapshot)),
                trailing: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    if (snapshot.passwordReadsBack)
                      IconButton(
                        tooltip: _revealPassword ? 'Hide' : 'Reveal',
                        icon: Icon(
                          _revealPassword
                              ? Icons.visibility_off_outlined
                              : Icons.visibility_outlined,
                          size: IconSize.sm,
                        ),
                        onPressed: () =>
                            setState(() => _revealPassword = !_revealPassword),
                      ),
                    IconButton(
                      tooltip: 'Set the password',
                      icon: const Icon(Icons.key_outlined, size: IconSize.sm),
                      onPressed: _busy ? null : _editPassword,
                    ),
                  ],
                ),
              ),
              InfoRow('State', wifiStateLabel(snapshot.state), labelWidth: 110),
              InfoRow(
                'IP address',
                snapshot.ip.isEmpty ? '-' : snapshot.ip,
                labelWidth: 110,
              ),
            ],
          ),
          const SizedBox(height: Spacing.lg),
          Text(
            'Every value above is the block\'s own: State and the address are computed by the '
            'device. Enabled and SSID are saved on the device after a write so they survive a '
            'reboot; the password is stored by the device itself and never read back.',
            style: Theme.of(context).textTheme.bodySmall,
          ),
        ],
      ),
    );
  }
}

/// The state the panel is in, stated plainly: what the device reports, and - for the block that
/// carries nothing - that WiFi is not implemented on this device rather than "disconnected".
class _WifiConditionBanner extends StatelessWidget {
  const _WifiConditionBanner({required this.snapshot});

  final WifiSnapshot snapshot;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final condition = snapshot.condition;
    final color = switch (wifiConditionTone(condition)) {
      WifiTone.success => kSuccess,
      WifiTone.neutral => kTextSecondary,
      WifiTone.warning => kWarning,
      WifiTone.error => kError,
    };
    final icon = switch (condition) {
      WifiCondition.connected => Icons.wifi,
      WifiCondition.connecting => Icons.wifi_find_outlined,
      WifiCondition.wrongPassword ||
      WifiCondition.error => Icons.wifi_off_outlined,
      WifiCondition.disabled => Icons.wifi_off_outlined,
      WifiCondition.noPassword => Icons.key_off_outlined,
      WifiCondition.noNetwork => Icons.wifi_tethering_off,
      WifiCondition.notImplemented => Icons.help_outline,
      WifiCondition.idle => Icons.wifi_tethering,
      WifiCondition.unknownState => Icons.question_mark,
    };
    return Card(
      color: kSurfaceAlt,
      child: Padding(
        padding: const EdgeInsets.all(Spacing.lg),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Icon(icon, color: color, size: IconSize.lg),
            const SizedBox(width: Spacing.md),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    wifiConditionHeadline(condition),
                    style: theme.textTheme.titleSmall?.copyWith(color: color),
                  ),
                  const SizedBox(height: Spacing.xs),
                  Text(
                    wifiConditionDetail(condition, snapshot),
                    style: theme.textTheme.bodySmall,
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}
