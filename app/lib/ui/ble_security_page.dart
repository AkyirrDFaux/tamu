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
/// register path — the radio's enable state, the passkey and the mode it implies, the advertised
/// name, the bond list and the pairing window, with nothing invented
/// (`core/ble_security.dart` holds the field map, the state rules, the bonds codec and the failure
/// guidance).
///
/// The block is SIX fields: `Enabled`, `Passkey`, `Mode`, `Display name`, `Bonds`, `Window`. The
/// panel draws them as three cards — the pairing settings, the bond list, the radio — because that
/// is how a user thinks about them; the fields themselves are shown in that order.
///
/// Three firmware rules shape the page:
///
///  - **Every write to this block is refused unless the frame arrived over USB.** Over a BLE link
///    the panel is therefore a read-out: the notice above the card says so up front, and a write
///    the device refuses comes back as a sentence naming the reason rather than a no-op.
///  - **The bond list is a VIEW of the bond store.** Its rows are decoded from one `Undefined`
///    array (three entries of `address(6) + name(16)`), so an entry can be renamed or deleted —
///    but a peer appears in it by PAIRING, and a non-zero address the device does not hold is
///    refused as a whole frame. The panel says that in the empty rows instead of offering an
///    "add" control the device would reject.
///  - **`Window` is a command and a read-out.** A write opens the window for that many
///    milliseconds and 0 closes it; the row shows the countdown the device reports. Only USB can
///    write it, which is the same rule as the rest of the block.
///
/// Advertising is on whenever the radio is up and enabled and no session is up (E64 ruling C);
/// the bond store and the pairing window decide which links become a session instead. The block is
/// compiled into the core only, so like the WiFi panel this page is reached only for a device whose
/// own block enumeration carries it, and it renders nothing when it does not.
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
            : 'The Bluetooth block is present but did not answer a read';
        _snapshot = answered ? BleSecuritySnapshot.fromReads(reads) : null;
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
      _snapshot = BleSecuritySnapshot.fromReads(_reads);
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
  /// into the device's `.SV` by a Save All, while the passkey (NVS) and the `Window`/`Bonds`
  /// triggers are applied by the device itself and need no save.
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
          : 'Display name set - the device advertises it now',
      valueHint: 'a display name is exactly $width characters',
    );
  }

  /// Writes the passkey (field 1). The device accepts exactly six ASCII digits and refuses the
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

  /// Writes `Window` (field 5): the milliseconds to open for, 0 to close. The device reads the
  /// field back as the time left, so the row shows the countdown the device reports rather than
  /// one the app counts.
  Future<void> _setWindow(int ms) => _writeField(
    bleFieldWindow,
    type: _writeType(bleFieldWindow, DataType.uint32.value),
    bytes: uint32ToBytes(ms).toList(),
    done: ms <= 0
        ? 'Pairing window closed'
        : 'Pairing window open for ${bleWindowCountdown(ms)}',
    valueHint: bleWindowProblem(ms, enabled: _snapshot?.enabled ?? false),
  );

  /// The window editor: a length in milliseconds, with the device's own limits stated and the
  /// 0-closes rule spelled out. The dialog refuses what the device would refuse, so a refusal is
  /// not the first the user hears of it.
  Future<void> _askWindow(int currentMs) async {
    final ms = await showDialog<int>(
      context: context,
      builder: (context) => _WindowDialog(
        currentMs: currentMs,
        enabled: _snapshot?.enabled ?? true,
        overUsb: _overUsb,
      ),
    );
    if (ms == null || !mounted) return;
    await _setWindow(ms);
  }

  /// Writes the whole `Bonds` array (field 4). The device requires the field's declared size for a
  /// non-Text value, so [bytes] is always [bleBondsLength] long — one write describes every slot.
  ///
  /// A refusal is decided by the device for the WHOLE frame and applies nothing (names included),
  /// so the sentence is built from what was sent against what the panel last read
  /// ([bleBondsRefusal]): a fabricated address is "a bond is made by pairing", while a list the
  /// device has moved on from asks for a re-read.
  Future<void> _writeBonds(
    List<int> bytes, {
    required List<BleBond> held,
    required String done,
  }) async {
    if (_busy) return;
    setState(() => _busy = true);
    final reply = await _client.writeBlockField(
      bleSettingsBlockTypeValue,
      0,
      bleFieldBonds,
      0,
      ValueInfo(
        type: _writeType(bleFieldBonds, DataType.undefined.value),
        size: bytes.length,
        flags: _writeFlags(bleFieldBonds),
      ),
      bytes,
    );
    if (!mounted) return;
    setState(() => _busy = false);
    _snack(
      reply != null
          ? done
          : bleBondsRefusal(overUsb: _overUsb, sent: bytes, held: held),
    );
    await _refresh();
  }

  /// Renames one entry's label. The array is written back whole with every other slot as it was
  /// read, because the device matches the frame slot by slot: an entry it does not recognise
  /// refuses the entire write.
  Future<void> _renameBond(int slot, String name) async {
    final snapshot = _snapshot;
    if (snapshot == null) return;
    await _writeBonds(
      bleBondsRenamed(snapshot.bonds, slot, name),
      held: snapshot.bonds,
      done: name.isEmpty
          ? 'Label cleared'
          : 'Label set to "$name" for ${snapshot.bonds[slot].addressText}',
    );
  }

  /// Clearing an entry's address deletes that bond — the only deletion the device offers, because
  /// the array is a view of the bond store. The consequence is stated before the tap: that phone
  /// has to pair again, and emptying the store makes the device open its automatic five-minute
  /// window.
  Future<void> _clearBond(BleBond bond) async {
    final snapshot = _snapshot;
    if (snapshot == null || bond.isEmpty) return;
    final slot = snapshot.bonds.indexOf(bond);
    final last = snapshot.bondCount == 1;
    final label = bond.name.isEmpty ? '' : ' (${bond.name})';
    final ok = await confirmDialog(
      context,
      title: 'Delete this bond?',
      body:
          'The device forgets ${bond.addressText}$label: that phone has to be forgotten in its '
          'own Bluetooth settings, and then pair again - which needs a pairing window.'
          '${last ? '\n\nThis is the last bond, so the store is emptied, and the device opens a '
                    'five-minute pairing window by itself afterwards.' : ''}'
          '${_overUsb ? '' : '\n\nThis app is connected over Bluetooth, so the device will refuse '
                    'the write. Connect over USB to delete a bond.'}',
      confirmLabel: 'Delete bond',
    );
    if (!ok || !mounted || slot < 0) return;
    await _writeBonds(
      bleBondsCleared(snapshot.bonds, slot),
      held: snapshot.bonds,
      done: last
          ? 'Bond deleted - the store is empty, so the device opens a five-minute pairing window'
          : 'Bond deleted - that phone has to pair again',
    );
  }

  /// The passkey editor: six digits, with the consequences in plain words above the field. The
  /// dialog is the confirmation - it states what the phone will ask, what `000000` means, and
  /// that the code can only be set again over USB.
  Future<void> _askPasskey({required bool currentSet}) async {
    final digits = await showDialog<String>(
      context: context,
      builder: (context) =>
          _PasskeyDialog(currentSet: currentSet, overUsb: _overUsb),
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
  /// block's "no passkey" value, so the panel says what it means. A field the device did not
  /// answer prints `-`: the default it fell back to is not a fact about the device.
  String _passkeyText(BleSecuritySnapshot snapshot) {
    if (!snapshot.answeredField(bleFieldPasskey)) return '-';
    if (!snapshot.passkeySet) return 'Not set - pairing uses Just Works';
    if (snapshot.passkey.isEmpty) return '-';
    return _revealPasskey ? snapshot.passkey : kSecretMask;
  }

  /// The mode row: the device's own `Mode`, or `-` when it did not answer it.
  String _modeText(BleSecuritySnapshot snapshot) {
    if (!snapshot.answeredField(bleFieldMode)) return '-';
    return snapshot.passkeySet
        ? '${bleModeLabel(snapshot.mode)} - a code is required'
        : '${bleModeLabel(snapshot.mode)} - no code required';
  }

  /// The label editor for one bond: sixteen characters, with what a label is (and is not) stated
  /// above the field.
  Future<void> _askBondName(int slot, String current) async {
    final name = await showDialog<String>(
      context: context,
      builder: (context) => _BondNameDialog(current: current),
    );
    if (name == null || !mounted || name == current) return;
    await _renameBond(slot, name);
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

    final bondsAnswered = snapshot.answeredField(bleFieldBonds);
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
              InfoRow('Mode', _modeText(snapshot), labelWidth: 120),
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
              // `Window` is a command and a read-out at once: the row shows the countdown the
              // device reports, and the editor writes it (0 closes).
              ListTile(
                dense: true,
                contentPadding: const EdgeInsets.symmetric(
                  horizontal: Spacing.xl,
                ),
                title: const Text('Pairing window'),
                subtitle: Text(_windowSubtitle(snapshot)),
                trailing: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    if (snapshot.answeredField(bleFieldWindow) &&
                        snapshot.windowOpen)
                      IconButton(
                        tooltip: 'Close the pairing window',
                        icon: const Icon(
                          Icons.wifi_tethering_off,
                          size: IconSize.sm,
                        ),
                        onPressed: _busy ? null : () => _setWindow(0),
                      ),
                    IconButton(
                      tooltip: 'Set the pairing window',
                      icon: const Icon(Icons.timer_outlined, size: IconSize.sm),
                      onPressed: _busy
                          ? null
                          : () => _askWindow(snapshot.windowMs),
                    ),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: Spacing.lg),
          // The bond list: one row per slot of the `Bonds` array. A row's label can be changed and
          // its address cleared (which deletes the bond); a peer is only ever ADDED by pairing,
          // which is what an empty slot says.
          InfoCard(
            title: 'Bonds',
            children: [
              if (!bondsAnswered)
                InfoRow('Bond list', '-', labelWidth: 120)
              else
                for (var slot = 0; slot < snapshot.bonds.length; slot++)
                  _BondRow(
                    bond: snapshot.bonds[slot],
                    busy: _busy,
                    onRename: () =>
                        _askBondName(slot, snapshot.bonds[slot].name),
                    onDelete: () => _clearBond(snapshot.bonds[slot]),
                  ),
              if (bleBondsDetail(snapshot) != null)
                Padding(
                  padding: const EdgeInsets.fromLTRB(
                    Spacing.xl,
                    Spacing.sm,
                    Spacing.xl,
                    Spacing.md,
                  ),
                  child: Text(
                    bleBondsDetail(snapshot)!,
                    style: Theme.of(context).textTheme.bodySmall,
                  ),
                ),
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
                  !snapshot.answeredField(bleFieldEnabled)
                      ? '-'
                      : snapshot.enabled
                      ? 'The device uses its radio'
                      : 'The radio is off: nothing is advertised and nobody can connect',
                ),
                value: snapshot.enabled,
                onChanged: _busy ? null : _setEnabled,
              ),
              // Display name IS the advertised name: there is no second read-out any more, and a
              // write re-advertises in the same loop pass, so the row says "now" and not "at the
              // next advertising start".
              ListTile(
                dense: true,
                contentPadding: const EdgeInsets.symmetric(
                  horizontal: Spacing.xl,
                ),
                title: const Text('Display name'),
                subtitle: Text(
                  !snapshot.answeredField(bleFieldDisplayName)
                      ? '-'
                      : snapshot.displayName.isEmpty
                      ? 'not set - the device advertises its own name'
                      : '${snapshot.displayName} - on the air now',
                ),
                trailing: IconButton(
                  tooltip: 'Set the display name',
                  icon: const Icon(Icons.edit_outlined, size: IconSize.sm),
                  onPressed: _busy
                      ? null
                      : () => _askDisplayName(snapshot.displayName),
                ),
              ),
            ],
          ),
          const SizedBox(height: Spacing.lg),
          Text(
            'Everything above is the block\'s own: the mode, the bond list, the window and the '
            'name are computed and reported by the device. The passkey is stored on the device '
            'itself and survives a firmware update; the bonds are kept in its bond store, so they '
            'are cleared by deleting an entry or by changing the passkey.',
            style: Theme.of(context).textTheme.bodySmall,
          ),
          const SizedBox(height: Spacing.md),
          // The stale-bond story, in the place the user is already looking when a phone will not
          // connect: it is the normal outcome of a firmware update, not a broken device - and
          // pairing again is a new pairing, so it needs a window (E64 ruling C).
          Text(
            'If this phone stops connecting: the device may no longer hold a bond for it, and a '
            'device that holds bonds ignores phones it has no bond for. Forget the device in '
            "your phone's Bluetooth settings, then pair again from a pairing window - open one "
            'here over USB.',
            style: Theme.of(context).textTheme.bodySmall,
          ),
        ],
      ),
    );
  }

  /// The window row's line: the countdown the device reports, plus what the state means. A window
  /// is the PAIRING state, so a closed one is never "off the air" - a bonded phone connects
  /// regardless.
  String _windowSubtitle(BleSecuritySnapshot snapshot) {
    if (!snapshot.answeredField(bleFieldWindow)) return '-';
    if (snapshot.windowOpen) {
      return '${bleWindowText(snapshot.windowMs)} - any phone may pair until it closes';
    }
    return 'closed - no new pairing until a window is open. A bonded phone connects anyway.';
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
      // E64 ruling C: an empty store is on the air and admits any phone, which is the state a
      // first pairing is made in - never a warning.
      BleSecurityCondition.closedNoBonds => kSuccess,
      BleSecurityCondition.noRadio => kWarning,
      BleSecurityCondition.incomplete => kWarning,
    };
    final icon = switch (condition) {
      BleSecurityCondition.windowOpen => Icons.bluetooth_searching,
      BleSecurityCondition.radioOff => Icons.bluetooth_disabled,
      BleSecurityCondition.closedBonded => Icons.bluetooth,
      BleSecurityCondition.closedNoBonds => Icons.bluetooth_searching,
      BleSecurityCondition.noRadio => Icons.help_outline,
      BleSecurityCondition.incomplete => Icons.help_outline,
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

/// One slot of the bond list: the peer's address, the label kept beside it, and the two edits the
/// device accepts — rename, and clear the address (which deletes the bond).
///
/// An empty slot is drawn too, and it says how a peer gets there: by pairing. That is the whole
/// reason the panel has no "add bond" control — the array is a view of the bond store, and the
/// device refuses a frame that names an address it does not hold.
class _BondRow extends StatelessWidget {
  const _BondRow({
    required this.bond,
    required this.busy,
    required this.onRename,
    required this.onDelete,
  });

  final BleBond bond;
  final bool busy;
  final VoidCallback onRename;
  final VoidCallback onDelete;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    if (bond.isEmpty) {
      return ListTile(
        dense: true,
        contentPadding: const EdgeInsets.symmetric(horizontal: Spacing.xl),
        leading: const Icon(Icons.bluetooth_outlined, size: IconSize.sm),
        title: const Text('Empty slot'),
        subtitle: Text(
          'A phone appears here once it has paired.',
          style: theme.textTheme.bodySmall,
        ),
      );
    }
    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.symmetric(horizontal: Spacing.xl),
      title: Text(bond.addressText),
      subtitle: Text(
        bond.name.isEmpty ? 'no label' : bond.name,
        style: theme.textTheme.bodySmall,
      ),
      trailing: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          IconButton(
            tooltip: 'Rename this bond',
            icon: const Icon(Icons.label_outline, size: IconSize.sm),
            onPressed: busy ? null : onRename,
          ),
          IconButton(
            tooltip: 'Delete this bond',
            icon: const Icon(Icons.link_off, size: IconSize.sm, color: kError),
            onPressed: busy ? null : onDelete,
          ),
        ],
      ),
    );
  }
}

/// The pairing-window editor: a length in milliseconds, with the device's own rule stated — a
/// write OPENS the window for that long and 0 closes it. The device refuses a non-zero value while
/// its radio is off and anything above `INT32_MAX` ms, so both are checked here before the write.
class _WindowDialog extends StatefulWidget {
  const _WindowDialog({
    required this.currentMs,
    required this.enabled,
    required this.overUsb,
  });

  /// The countdown the device reported when the dialog opened, offered as the starting value.
  final int currentMs;

  /// Whether the radio is on: a non-zero window cannot open with it off.
  final bool enabled;

  /// Whether this app's link is USB. A write is refused on any other link, so the dialog says so
  /// before a length is typed.
  final bool overUsb;

  @override
  State<_WindowDialog> createState() => _WindowDialogState();
}

class _WindowDialogState extends State<_WindowDialog> {
  late final _controller = TextEditingController(
    text: widget.currentMs > 0 ? '${widget.currentMs}' : '',
  );

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  int? get _ms => int.tryParse(_controller.text.trim());

  void _preset(int ms) => setState(() => _controller.text = '$ms');

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final ms = _ms;
    final problem = ms == null
        ? 'enter the length in milliseconds (0 closes the window)'
        : bleWindowProblem(ms, enabled: widget.enabled);
    return AlertDialog(
      title: const Text('Pairing window'),
      content: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              'While a window is open any phone may pair with the device. A bonded phone '
              'connects at any time, window or not.',
              style: theme.textTheme.bodySmall,
            ),
            const SizedBox(height: Spacing.sm),
            Text(
              'The write opens the window for the number of milliseconds you enter; 0 closes it '
              'now.',
              style: theme.textTheme.bodySmall,
            ),
            if (!widget.overUsb) ...[
              const SizedBox(height: Spacing.md),
              Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Icon(Icons.usb, size: IconSize.sm, color: kWarning),
                  const SizedBox(width: Spacing.sm),
                  Expanded(
                    child: Text(
                      'This app is connected over Bluetooth: the device will refuse the write. '
                      'Connect over USB to open a window.',
                      style: theme.textTheme.bodySmall,
                    ),
                  ),
                ],
              ),
            ],
            const SizedBox(height: Spacing.lg),
            TextField(
              controller: _controller,
              autofocus: true,
              keyboardType: TextInputType.number,
              inputFormatters: [FilteringTextInputFormatter.digitsOnly],
              onChanged: (_) => setState(() {}),
              decoration: const InputDecoration(
                labelText: 'Milliseconds',
                suffixText: 'ms',
              ),
            ),
            const SizedBox(height: Spacing.sm),
            Wrap(
              spacing: Spacing.sm,
              children: [
                ActionChip(
                  label: const Text('30 s'),
                  onPressed: () => _preset(30000),
                ),
                ActionChip(
                  label: const Text('1 min'),
                  onPressed: () => _preset(60000),
                ),
                ActionChip(
                  label: const Text('5 min'),
                  onPressed: () => _preset(300000),
                ),
              ],
            ),
            if (problem != null) ...[
              const SizedBox(height: Spacing.sm),
              Text(
                problem,
                style: theme.textTheme.bodySmall?.copyWith(color: kWarning),
              ),
            ],
          ],
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('Cancel'),
        ),
        FilledButton(
          onPressed: problem == null && ms != null
              ? () => Navigator.pop(context, ms)
              : null,
          child: const Text('Set window'),
        ),
      ],
    );
  }
}

/// The label editor for one bond. The label is the app's own — the NimBLE bond store keeps no peer
/// name (`BleBondLabelStore.h`) — so the copy says what it is for rather than implying the phone
/// reports it.
class _BondNameDialog extends StatefulWidget {
  const _BondNameDialog({required this.current});

  final String current;

  @override
  State<_BondNameDialog> createState() => _BondNameDialogState();
}

class _BondNameDialogState extends State<_BondNameDialog> {
  late final _controller = TextEditingController(text: widget.current);

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('Bond label'),
      content: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              'A name for this phone, kept on the device beside its address so two bonded phones '
              'can be told apart. The phone never sends one. Leave it empty to remove the label.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
            const SizedBox(height: Spacing.lg),
            TextField(
              controller: _controller,
              autofocus: true,
              maxLength: bleBondNameLength,
              decoration: const InputDecoration(hintText: 'Phone name'),
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
          child: const Text('Set label'),
        ),
      ],
    );
  }
}

/// The passkey editor: six digits, and the consequences stated in plain words above the field.
/// The dialog IS the confirmation - it says what the phone will ask, what `000000` means, and
/// that a forgotten code can only be replaced over USB.
class _PasskeyDialog extends StatefulWidget {
  const _PasskeyDialog({required this.currentSet, required this.overUsb});

  /// Whether a code is set now, so the copy can say the old one stops working.
  final bool currentSet;

  /// Whether this app's link to the device is USB. The device refuses the write from any other
  /// link, so the dialog says so before the code is typed instead of after a refusal.
  final bool overUsb;

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
            if (!widget.overUsb) ...[
              const SizedBox(height: Spacing.md),
              Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Icon(Icons.usb, size: IconSize.sm, color: kWarning),
                  const SizedBox(width: Spacing.sm),
                  Expanded(
                    child: Text(
                      'This app is connected over Bluetooth: the device will refuse the write. '
                      'Connect over USB to set the code.',
                      style: theme.textTheme.bodySmall,
                    ),
                  ),
                ],
              ),
            ],
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
