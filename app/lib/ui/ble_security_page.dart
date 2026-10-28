import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show FilteringTextInputFormatter;

import '../core/ble_security.dart';
import '../core/connection.dart';
import '../core/register_client.dart';
import '../core/transport.dart' show UsbTransport;
import '../core/types.dart';
import 'formatters.dart' show kSecretMask;
import 'theme.dart';
import 'widgets.dart';

/// The Bluetooth panel: the `BLE Settings` block (BlockType `0x0B`) read and written through the
/// register path — the pairing mode, the bonded phones, the connectable window, the passkey, the
/// radio's enable state and the advertised name, with nothing invented
/// (`core/ble_security.dart` holds the field map, the state rules and the failure guidance).
///
/// Two firmware rules shape the page:
///
///  - **Every write to this block is refused unless the frame arrived over USB.** Over a BLE link
///    the panel is therefore a read-out: the notice above the card says so up front, and a write
///    the device refuses comes back as a sentence naming the reason rather than a no-op.
///  - **The device is discoverable only inside a pairing window**, and it opens one by itself for
///    five minutes whenever it holds no bonds. The panel shows the countdown, the bond list and
///    the state as one story so "it will not connect" has a visible cause.
///
/// The block is compiled into the core only, so like the WiFi panel this page is reached only for
/// a device whose own block enumeration carries it, and it renders nothing when it does not.
class BleSecurityPage extends StatefulWidget {
  final int deviceId;

  /// Test seam: the refresh paths only run on a live link, which a widget test cannot fake.
  final bool Function()? isConnected;

  /// Test seam: whether this app's own link to the device is USB, the only link the block
  /// accepts a write on.
  final bool Function()? isUsbLink;

  /// Test seam: how the page builds its register client. Defaults to a real [RegisterClient].
  final RegisterClient Function(int deviceId)? clientFactory;

  const BleSecurityPage({
    super.key,
    required this.deviceId,
    this.isConnected,
    this.isUsbLink,
    this.clientFactory,
  });

  @override
  State<BleSecurityPage> createState() => _BleSecurityPageState();
}

class _BleSecurityPageState extends State<BleSecurityPage>
    with AutoRefreshMixin<BleSecurityPage> {
  late final RegisterClient _client =
      (widget.clientFactory ?? (int id) => RegisterClient(deviceId: id))(
        widget.deviceId,
      );

  /// The last reading of the block, or null before the first successful one.
  BleSecuritySnapshot? _snapshot;

  /// The raw field reads behind [_snapshot]: the write path takes a field's declared type and
  /// width from here instead of guessing them.
  Map<int, BleFieldRead> _reads = const {};

  /// The Bond field's keyed reads (0..[bleBondSlots]-1), kept beside [_reads] because the field is
  /// read per key rather than per field number.
  Map<int, BleFieldRead> _bondReads = const {};

  /// False when the device's enumeration does not carry the block: the page then draws nothing.
  bool _present = true;

  bool _refreshing = false;
  bool _busy = false;
  bool _revealPasskey = false;
  String? _error;

  /// Ticks the pairing-window countdown while a window is open. One field read a second is enough
  /// to keep it honest.
  Timer? _windowTimer;

  bool get _connected =>
      (widget.isConnected ?? (() => ConnectionManager.instance.isConnected))();

  /// True when this app talks to the device over USB, the only link the block accepts a write on.
  bool get _overUsb =>
      (widget.isUsbLink ??
      () => ConnectionManager.instance.transport is UsbTransport)();

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  void dispose() {
    _windowTimer?.cancel();
    super.dispose();
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
      if (!hasBleSettingsBlock(types)) {
        // No block, no panel: a device without the block is the normal case, not a failure.
        setState(() {
          _refreshing = false;
          _present = false;
          _error = null;
          _snapshot = null;
        });
        return;
      }

      final reads = <int, BleFieldRead>{};
      for (var field = 0; field < bleFieldCount; field++) {
        reads[field] = await _client.readBlockField(
          bleSettingsBlockTypeValue,
          0,
          field,
          0,
        );
      }
      // The Bond field is keyed: one address per slot, oldest first.
      final bonds = <int, BleFieldRead>{};
      for (var key = 0; key < bleBondSlots; key++) {
        bonds[key] = await _client.readBlockField(
          bleSettingsBlockTypeValue,
          0,
          bleFieldBond,
          key,
        );
      }
      if (!mounted) return;
      final answered = reads.values.any((r) => r != null);
      setState(() {
        _refreshing = false;
        _present = true;
        _reads = reads;
        _bondReads = bonds;
        // Present but silent is a failure worth showing: the device claims a block it will not
        // let anyone read, which is not the same thing as not having one.
        _error = answered
            ? null
            : 'The Bluetooth block is present but did not answer a read';
        _snapshot = answered
            ? BleSecuritySnapshot.fromReads(reads, bonds: bonds)
            : null;
      });
      _syncWindowTimer();
    } finally {
      if (mounted && _refreshing) setState(() => _refreshing = false);
    }
  }

  /// Starts the countdown tick when a window is open, stops it when it closes.
  void _syncWindowTimer() {
    final left = _snapshot?.windowMs ?? 0;
    if (left > 0 && _windowTimer == null) {
      _windowTimer = Timer.periodic(
        const Duration(seconds: 1),
        (_) => _tickWindow(),
      );
    } else if (left <= 0) {
      _windowTimer?.cancel();
      _windowTimer = null;
    }
  }

  /// One countdown tick: ask the device for the `Window` field alone (it is the authority on how
  /// much is left), fold the answer into the snapshot and stop ticking once it reports nothing.
  Future<void> _tickWindow() async {
    if (!_connected) {
      _windowTimer?.cancel();
      _windowTimer = null;
      return;
    }
    final read = await _client.readBlockField(
      bleSettingsBlockTypeValue,
      0,
      bleFieldWindow,
      0,
    );
    if (!mounted) return;
    if (read == null) return; // a lost answer is not a closed window
    setState(() {
      _reads = {..._reads, bleFieldWindow: read};
      _snapshot = BleSecuritySnapshot.fromReads(_reads, bonds: _bondReads);
    });
    if ((_snapshot?.windowMs ?? 0) <= 0) {
      _windowTimer?.cancel();
      _windowTimer = null;
    }
  }

  /// The write's declared type: the device's own for a field it just answered for, and the
  /// block's documented type for one it did not.
  int _writeType(int field, int fallback) =>
      _reads[field]?.meta.type ?? fallback;

  /// The flags a write describes the field with: what the device just reported, else the block's
  /// own treatment (a trigger, persistent for the two settings that are mirrored into `.SV`).
  int _writeFlags(int field, {bool persistent = false}) =>
      _reads[field]?.meta.flags ??
      (persistent
          ? ValueFlags.persistent | ValueFlags.trigger
          : ValueFlags.trigger);

  /// Sends one write and reports the outcome in plain words. A refusal is a normal `FLAG_FAIL`
  /// reply (`null` from the client), so it is surfaced — never silently dropped.
  ///
  /// [persist] saves the persistent space afterwards: `Enabled` and `Display name` are mirrored
  /// into the device's `.SV` by a Save All, while the passkey (NVS) and the two commands are
  /// applied by their triggers and need no save.
  Future<void> _writeField(
    int field, {
    required int type,
    required List<int> bytes,
    required String done,
    bool persistent = false,
    bool persist = false,
    String? valueHint,
  }) async {
    if (_busy) return;
    setState(() => _busy = true);
    final reply = await _client.writeBlockField(
      bleSettingsBlockTypeValue,
      0,
      field,
      0,
      ValueInfo(
        type: type,
        size: bytes.length,
        flags: _writeFlags(field, persistent: persistent),
      ),
      bytes,
    );
    final wrote = reply != null;
    final saved = wrote && persist && await _client.saveAll();
    if (!mounted) return;
    setState(() {
      _busy = false;
      _revealPasskey = false;
    });
    _snack(
      !wrote
          ? bleWriteRefusal(overUsb: _overUsb, valueHint: valueHint)
          : !persist
          ? done
          : saved
          ? done
          : '$done - but the device did not save it',
    );
    await _refresh();
  }

  Future<void> _setEnabled(bool enabled) => _writeField(
    bleFieldEnabled,
    type: _writeType(bleFieldEnabled, DataType.bool_.value),
    bytes: [enabled ? 1 : 0],
    persistent: true,
    persist: true,
    done: enabled ? 'Radio switched on' : 'Radio switched off',
  );

  Future<void> _setDisplayName(String name) {
    final bytes = name.codeUnits.toList();
    final width = _reads[bleFieldDisplayName]?.meta.size ?? bleDisplayNameWidth;
    if (bytes.length >= width) {
      bytes.length = width;
    } else {
      bytes.addAll(List<int>.filled(width - bytes.length, 0));
    }
    return _writeField(
      bleFieldDisplayName,
      type: _writeType(bleFieldDisplayName, DataType.text.value),
      bytes: bytes,
      persistent: true,
      persist: true,
      done: name.isEmpty
          ? 'Display name cleared - the device advertises its own name'
          : 'Display name set',
      valueHint: 'a display name is exactly $width characters',
    );
  }

  /// Writes the passkey (field 0). The device accepts exactly six ASCII digits and refuses the
  /// write otherwise, so the shape is checked here first and the refusal still has a sentence.
  Future<void> _setPasskey(String digits) => _writeField(
    bleFieldPasskey,
    type: _writeType(bleFieldPasskey, DataType.text.value),
    bytes: digits.codeUnits.toList(),
    done: digits == blePasskeyNone
        ? 'Passkey cleared - the device pairs with Just Works again'
        : 'Passkey set - the phone will ask for this code when it next pairs',
    valueHint: 'the passkey must be exactly $blePasskeyLength digits',
  );

  /// `Action = 0`: opens (or extends) the connectable window. Accepted over USB only, like every
  /// write to this block.
  Future<void> _openWindow() => _writeField(
    bleFieldAction,
    type: _writeType(bleFieldAction, DataType.enum_.value),
    bytes: const [bleActionOpenWindow],
    done: 'Pairing window open',
    valueHint: 'the device could not open a window',
  );

  /// `Action = 1`: forgets every bond. Destructive, so the consequence is stated before the tap.
  Future<void> _forgetBonds() async {
    final snapshot = _snapshot;
    final count = snapshot?.bondCount ?? 0;
    final ok = await confirmDialog(
      context,
      title: 'Forget all bonds?',
      body: count == 0
          ? 'The device holds no bonds, so there is nothing to forget. The command still '
                'resets its store.'
          : 'Every phone paired with this device ($count today) must pair again: each one has '
                'to be forgotten in that phone\'s Bluetooth settings before it can connect. '
                'The device opens a five-minute pairing window by itself afterwards, so it stays '
                'reachable.',
      confirmLabel: 'Forget bonds',
    );
    if (!ok || !mounted) return;
    await _writeField(
      bleFieldAction,
      type: _writeType(bleFieldAction, DataType.enum_.value),
      bytes: const [bleActionForgetBonds],
      done: 'Bonds forgotten - every phone must pair again',
      valueHint: 'the device could not clear its bond store',
    );
  }

  /// The passkey editor: six digits, with the consequences in plain words above the field. The
  /// dialog is the confirmation - it states what the phone will ask, what `000000` means, and
  /// that the code can only be set again over USB.
  Future<void> _askPasskey({required bool currentSet}) async {
    final digits = await showDialog<String>(
      context: context,
      builder: (context) => _PasskeyDialog(currentSet: currentSet),
    );
    if (digits == null || !mounted) return;
    await _setPasskey(digits);
  }

  /// Clearing the passkey is its own confirmation: it is a security decision, not an edit.
  Future<void> _clearPasskey() async {
    final ok = await confirmDialog(
      context,
      title: 'Clear the passkey?',
      body:
          'The device returns to Just Works: pairing will not ask for a code any more, and '
          'anyone in range can pair while a pairing window is open. Set a new code here over '
          'USB when you want the protection back.',
      confirmLabel: 'Clear passkey',
    );
    if (!ok || !mounted) return;
    await _setPasskey(blePasskeyNone);
  }

  Future<void> _askDisplayName(String current) async {
    final name = await showDialog<String>(
      context: context,
      builder: (context) => _DisplayNameDialog(current: current),
    );
    if (name == null || !mounted || name == current) return;
    await _setDisplayName(name);
  }

  /// What the passkey row prints. `000000` is not a code the user has to remember, it is the
  /// block's "no passkey" value, so the panel says what it means.
  String _passkeyText(BleSecuritySnapshot snapshot) {
    if (!snapshot.passkeySet) return 'Not set - pairing uses Just Works';
    if (snapshot.passkey.isEmpty) return '-';
    return _revealPasskey ? snapshot.passkey : kSecretMask;
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
    if (!_present) return TamuPage(title: 'Bluetooth', actions: actions);

    final snapshot = _snapshot;
    if (_error != null && snapshot == null) {
      return TamuPage(
        title: 'Bluetooth',
        actions: actions,
        state: StateKind.error,
        stateMessage: 'Bluetooth block unavailable',
        stateDetails: _error,
        stateAction: OutlinedButton(
          onPressed: _refresh,
          child: const Text('Retry'),
        ),
      );
    }
    if (snapshot == null) {
      return TamuPage(
        title: 'Bluetooth',
        actions: actions,
        state: StateKind.loading,
        stateMessage: 'Reading Bluetooth settings',
      );
    }

    return TamuPage(
      title: 'Bluetooth',
      actions: actions,
      child: ListView(
        padding: Insets.page,
        children: [
          _BleConditionBanner(snapshot: snapshot),
          if (!_overUsb) ..._overBleNotice(context),
          const SizedBox(height: Spacing.lg),
          InfoCard(
            title: 'Pairing',
            children: [
              InfoRow(
                'Mode',
                snapshot.passkeySet
                    ? '${bleModeLabel(snapshot.mode)} - a code is required'
                    : '${bleModeLabel(snapshot.mode)} - no code required',
                labelWidth: 120,
              ),
              ListTile(
                dense: true,
                contentPadding: const EdgeInsets.symmetric(
                  horizontal: Spacing.xl,
                ),
                title: const Text('Passkey'),
                subtitle: Text(_passkeyText(snapshot)),
                trailing: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    if (snapshot.passkeySet)
                      IconButton(
                        tooltip: _revealPasskey ? 'Hide' : 'Show the code',
                        icon: Icon(
                          _revealPasskey
                              ? Icons.visibility_off_outlined
                              : Icons.visibility_outlined,
                          size: IconSize.sm,
                        ),
                        onPressed: () =>
                            setState(() => _revealPasskey = !_revealPasskey),
                      ),
                    IconButton(
                      tooltip: 'Set the passkey',
                      icon: const Icon(Icons.key_outlined, size: IconSize.sm),
                      onPressed: _busy
                          ? null
                          : () => _askPasskey(currentSet: snapshot.passkeySet),
                    ),
                    if (snapshot.passkeySet)
                      IconButton(
                        tooltip: 'Clear the passkey',
                        icon: const Icon(
                          Icons.key_off_outlined,
                          size: IconSize.sm,
                        ),
                        onPressed: _busy ? null : _clearPasskey,
                      ),
                  ],
                ),
              ),
              InfoRow(
                'Pairing window',
                bleWindowText(snapshot.windowMs),
                labelWidth: 120,
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(
                  Spacing.xl,
                  Spacing.sm,
                  Spacing.xl,
                  Spacing.sm,
                ),
                child: Wrap(
                  spacing: Spacing.md,
                  runSpacing: Spacing.sm,
                  children: [
                    OutlinedButton.icon(
                      onPressed: _busy ? null : _openWindow,
                      icon: const Icon(Icons.wifi_tethering, size: IconSize.sm),
                      label: const Text('Open pairing window'),
                    ),
                    OutlinedButton.icon(
                      onPressed: _busy ? null : _forgetBonds,
                      icon: const Icon(
                        Icons.link_off,
                        size: IconSize.sm,
                        color: kError,
                      ),
                      label: const Text('Forget all bonds'),
                    ),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: Spacing.lg),
          InfoCard(
            title: 'Bonded devices',
            children: [
              InfoRow('Bond count', '${snapshot.bondCount}', labelWidth: 120),
              if (snapshot.bonds.isEmpty)
                Padding(
                  padding: const EdgeInsets.fromLTRB(
                    Spacing.xl,
                    Spacing.xs,
                    Spacing.xl,
                    Spacing.md,
                  ),
                  child: Text(
                    snapshot.bondCount == 0
                        ? 'No phone is paired with this device.'
                        : 'The device reports ${snapshot.bondCount} bonds but did not read '
                              'their addresses back.',
                    style: Theme.of(context).textTheme.bodySmall,
                  ),
                )
              else
                for (final address in snapshot.bonds)
                  // Address only: the device's bond store keeps no peer name (Issues.md E60),
                  // so a name here would be invented.
                  InfoRow('Paired phone', address, labelWidth: 120),
            ],
          ),
          const SizedBox(height: Spacing.lg),
          InfoCard(
            title: 'Radio',
            children: [
              SwitchListTile(
                dense: true,
                contentPadding: const EdgeInsets.symmetric(
                  horizontal: Spacing.xl,
                ),
                title: const Text('Enabled'),
                subtitle: Text(
                  snapshot.enabled
                      ? 'The device uses its radio'
                      : 'The radio is off: nothing is advertised and nobody can connect',
                ),
                value: snapshot.enabled,
                onChanged: _busy ? null : _setEnabled,
              ),
              ListTile(
                dense: true,
                contentPadding: const EdgeInsets.symmetric(
                  horizontal: Spacing.xl,
                ),
                title: const Text('Display name'),
                subtitle: Text(
                  snapshot.displayName.isEmpty
                      ? 'not set - the device uses its own name'
                      : snapshot.displayName,
                ),
                trailing: IconButton(
                  tooltip: 'Set the display name',
                  icon: const Icon(Icons.edit_outlined, size: IconSize.sm),
                  onPressed: _busy
                      ? null
                      : () => _askDisplayName(snapshot.displayName),
                ),
              ),
              InfoRow(
                'Advertising as',
                snapshot.advertisedName.isEmpty ? '-' : snapshot.advertisedName,
                labelWidth: 120,
              ),
            ],
          ),
          const SizedBox(height: Spacing.lg),
          Text(
            'Everything above is the block\'s own: the mode, the bond list, the window and the '
            'advertised name are computed and reported by the device. The passkey is stored on '
            'the device itself and survives a firmware update; the bonds do too, so they can '
            'only be cleared with "Forget all bonds".',
            style: Theme.of(context).textTheme.bodySmall,
          ),
          const SizedBox(height: Spacing.md),
          // The stale-bond story, in the place the user is already looking when a phone will not
          // connect: it is the normal outcome of a firmware update, not a broken device.
          Text(
            'If this phone stops connecting: the device may no longer hold a bond for it. '
            'Forget the device in your phone\'s Bluetooth settings and pair again.',
            style: Theme.of(context).textTheme.bodySmall,
          ),
        ],
      ),
    );
  }

  /// The up-front statement that the whole block is read-only over this link, so the user is not
  /// left filling a form the device will refuse.
  List<Widget> _overBleNotice(BuildContext context) => [
    const SizedBox(height: Spacing.md),
    Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Icon(Icons.usb, size: IconSize.sm, color: kWarning),
        const SizedBox(width: Spacing.sm),
        Expanded(
          child: Text(
            bleWriteRestriction(overUsb: false)!,
            style: Theme.of(context).textTheme.bodySmall,
          ),
        ),
      ],
    ),
  ];
}

/// The state the panel is in, stated plainly: what the device reports about its radio and its
/// pairing window, so "it will not connect" has a visible cause.
class _BleConditionBanner extends StatelessWidget {
  const _BleConditionBanner({required this.snapshot});

  final BleSecuritySnapshot snapshot;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final condition = snapshot.condition;
    final color = switch (condition) {
      BleSecurityCondition.windowOpen => kSuccess,
      BleSecurityCondition.radioOff => kWarning,
      BleSecurityCondition.closedBonded => kTextSecondary,
      BleSecurityCondition.closedNoBonds => kWarning,
      BleSecurityCondition.noRadio => kWarning,
    };
    final icon = switch (condition) {
      BleSecurityCondition.windowOpen => Icons.bluetooth_searching,
      BleSecurityCondition.radioOff => Icons.bluetooth_disabled,
      BleSecurityCondition.closedBonded => Icons.bluetooth,
      BleSecurityCondition.closedNoBonds => Icons.bluetooth_disabled,
      BleSecurityCondition.noRadio => Icons.help_outline,
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
                    bleSecurityHeadline(condition),
                    style: theme.textTheme.titleSmall?.copyWith(color: color),
                  ),
                  const SizedBox(height: Spacing.xs),
                  Text(
                    bleSecurityDetail(condition, snapshot),
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

/// The guidance the app shows after a failed BLE attempt: what most likely happened and what to
/// do about it, with the platform's own words kept underneath for a bug report.
Future<void> showBleLinkGuidance(
  BuildContext context,
  BleLinkGuidance guidance, {
  String? linkError,
}) => showDialog<void>(
  context: context,
  builder: (context) => AlertDialog(
    title: Text(guidance.headline),
    content: SingleChildScrollView(
      child: _BleGuidanceBody(guidance: guidance, linkError: linkError),
    ),
    actions: [
      FilledButton(
        onPressed: () => Navigator.pop(context),
        child: const Text('Got it'),
      ),
    ],
  ),
);

class _BleGuidanceBody extends StatelessWidget {
  const _BleGuidanceBody({required this.guidance, this.linkError});

  final BleLinkGuidance guidance;
  final String? linkError;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        Text(guidance.detail, style: theme.textTheme.bodyMedium),
        const SizedBox(height: Spacing.lg),
        for (var i = 0; i < guidance.steps.length; i++)
          Padding(
            padding: const EdgeInsets.only(bottom: Spacing.sm),
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('${i + 1}.', style: theme.textTheme.bodySmall),
                const SizedBox(width: Spacing.sm),
                Expanded(child: Text(guidance.steps[i])),
              ],
            ),
          ),
        if (linkError != null && linkError!.trim().isNotEmpty) ...[
          const SizedBox(height: Spacing.md),
          Text('The link said:', style: theme.textTheme.bodySmall),
          Text(linkError!, style: theme.textTheme.bodySmall),
        ],
      ],
    );
  }
}

/// The passkey editor: six digits, and the consequences stated in plain words above the field.
/// The dialog IS the confirmation - it says what the phone will ask, what `000000` means, and
/// that a forgotten code can only be replaced over USB.
class _PasskeyDialog extends StatefulWidget {
  const _PasskeyDialog({required this.currentSet});

  /// Whether a code is set now, so the copy can say the old one stops working.
  final bool currentSet;

  @override
  State<_PasskeyDialog> createState() => _PasskeyDialogState();
}

class _PasskeyDialogState extends State<_PasskeyDialog> {
  final _controller = TextEditingController();

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final digits = _controller.text;
    return AlertDialog(
      title: const Text('Set the passkey'),
      content: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              widget.currentSet
                  ? 'The phone will ask for the new code the next time it pairs with this '
                        'device. The code that is set now stops working.'
                  : 'The phone will ask for this code the next time it pairs with this device, '
                        'and so will every other phone.',
              style: theme.textTheme.bodySmall,
            ),
            const SizedBox(height: Spacing.md),
            Text(
              '$blePasskeyNone returns the device to Just Works, which pairs with no code at '
              'all.',
              style: theme.textTheme.bodySmall,
            ),
            const SizedBox(height: Spacing.md),
            Text(
              'If you forget the code you must set a new one over USB: a phone that cannot pair '
              'cannot change it.',
              style: theme.textTheme.bodySmall,
            ),
            const SizedBox(height: Spacing.lg),
            TextField(
              controller: _controller,
              autofocus: true,
              keyboardType: TextInputType.number,
              inputFormatters: [FilteringTextInputFormatter.digitsOnly],
              maxLength: blePasskeyLength,
              onChanged: (_) => setState(() {}),
              decoration: const InputDecoration(
                labelText: 'Six digits',
                hintText: '123456',
              ),
            ),
          ],
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('Cancel'),
        ),
        FilledButton(
          onPressed: blePasskeyIsValid(digits)
              ? () => Navigator.pop(context, digits)
              : null,
          child: const Text('Set passkey'),
        ),
      ],
    );
  }
}

/// The advertised-name editor: empty means "advertise the device's own name", which the copy says
/// so an empty field is a choice rather than a mistake.
class _DisplayNameDialog extends StatefulWidget {
  const _DisplayNameDialog({required this.current});

  final String current;

  @override
  State<_DisplayNameDialog> createState() => _DisplayNameDialogState();
}

class _DisplayNameDialogState extends State<_DisplayNameDialog> {
  late final _controller = TextEditingController(text: widget.current);

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('Display name'),
      content: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              'The name phones see when they scan. Leave it empty and the device advertises its '
              'own name from the System block instead.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
            const SizedBox(height: Spacing.lg),
            TextField(
              controller: _controller,
              autofocus: true,
              maxLength: bleDisplayNameWidth,
              decoration: const InputDecoration(hintText: 'Device name'),
            ),
          ],
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('Cancel'),
        ),
        FilledButton(
          onPressed: () => Navigator.pop(context, _controller.text.trim()),
          child: const Text('Set name'),
        ),
      ],
    );
  }
}
