import 'dart:async';

import 'package:flutter/material.dart';

import '../core/register_client.dart';
import '../core/types.dart';
import '../core/wifi_settings.dart';
import '../core/notice.dart';
import 'block_panel.dart';
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

class _WifiPageState extends State<WifiPage>
    with
        AutoRefreshMixin<WifiPage>,
        BlockPanelRefresh<WifiPage, WifiSnapshot> {
  late final RegisterClient _client =
      (widget.clientFactory ?? (int id) => RegisterClient(deviceId: id))(
        widget.deviceId,
      );

  bool _busy = false;
  bool _revealPassword = false;

  bool get _connected => blockPanelConnected(widget.isConnected);

  /// True when this app talks to the device over USB, which is the only link the block accepts
  /// a write on.
  bool get _overUsb => blockPanelOverUsb(widget.isUsbLink);

  // What this panel is, for the shared refresh ([BlockPanelRefresh]): the `WiFi Settings` block,
  // five fields, and a reading built from the fields the device answered.

  @override
  RegisterClient get blockClient => _client;

  @override
  bool get blockConnected => _connected;

  @override
  bool get blockBusy => _busy;

  @override
  bool hasBlock(Iterable<({int type, int maxInstance})> types) =>
      hasWifiSettingsBlock(types);

  @override
  int get blockType => wifiSettingsBlockTypeValue;

  @override
  int get blockFieldCount => wifiFieldCount;

  @override
  String get blockName => 'WiFi';

  @override
  WifiSnapshot snapshotOf(Map<int, BlockFieldRead> reads) =>
      WifiSnapshot.fromFields(reads);

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();

  Future<void> _refresh() => refreshBlock();

  /// The write's declared type: the device's own for a field it just answered for, and the
  /// block's documented type for one it did not (Enabled is a Bool, the rest are Text).
  int _writeType(int field) =>
      blockReads[field]?.meta.type ??
      (field == wifiFieldEnabled ? DataType.bool_.value : DataType.text.value);

  /// A text value at the width the device declared. The firmware pads a SHORTER Text write
  /// itself, but refuses to pad a field wider than its 32-byte staging buffer, so the app sends
  /// the full width: both rules then hold whatever the field's size is.
  List<int> _wireText(int field, String text) {
    final bytes = text.codeUnits.toList();
    final size = blockReads[field]?.meta.size ?? 0;
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
        flags: blockReads[field]?.meta.flags ?? ValueFlags.persistent,
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
    showNotice(
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
    // The block is missing: draw the panel as nothing at all (not an empty card, not an error).
    final statePage = blockPage(
      title: 'WiFi',
      unavailable: 'WiFi block unavailable',
      reading: 'Reading WiFi settings',
    );
    if (statePage != null) return statePage;

    final snapshot = blockSnapshot!;

    return TamuPage(
      title: 'WiFi',
      actions: blockActions,
      child: ListView(
        padding: Insets.page,
        children: [
          _WifiConditionBanner(snapshot: snapshot),
          // The device refuses a write that arrived over BLE (firmware WifiSettingsGlue.h), so
          // say it before the form is filled rather than after a rejected write.
          if (!_overUsb)
            ...linkNotice(
              context,
              'Changing these settings needs a USB link to the device; over any other link the '
              'device refuses the write.',
            ),
          const SizedBox(height: Spacing.lg),
          InfoCard(
            title: 'WiFi Settings',
            children: [
              blockPanelSwitch(
                title: 'Enabled',
                subtitle: snapshot.enabled
                    ? 'The radio is used'
                    : 'The radio is off',
                value: snapshot.enabled,
                onChanged: _busy ? null : _setEnabled,
              ),
              blockPanelRow(
                title: 'SSID',
                subtitle: snapshot.ssid.isEmpty ? 'not set' : snapshot.ssid,
                trailing: IconButton(
                  tooltip: 'Set the SSID',
                  icon: const Icon(Icons.edit_outlined, size: IconSize.sm),
                  onPressed: _busy ? null : () => _editSsid(snapshot.ssid),
                ),
              ),
              blockPanelRow(
                title: 'Password',
                subtitle: _passwordText(snapshot),
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
    return ConditionBanner(
      color: color,
      icon: icon,
      headline: wifiConditionHeadline(condition),
      detail: wifiConditionDetail(condition, snapshot),
    );
  }
}
