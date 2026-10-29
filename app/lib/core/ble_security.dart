/// The `BLE Settings` block (BlockType `0x0B`, core only): the BLE link's settings and state as
/// register fields, so the panel in the app reads and writes them through the same register path
/// as every other block instead of a side-channel command set.
///
/// The block's table order is fixed here because the panel, the block registry and the firmware
/// contract test all address the fields by number:
///
///   0 Enabled          Bool, persistent+write       false = the radio is not used at all
///   1 Passkey          Text(6), write (Trigger)     6 ASCII digits, "000000" = Just Works
///   2 Mode             Enum, read-only              0 Just Works, 1 Passkey, derived from the passkey
///   3 Display name     Text(16), persistent+write   the advertised name, empty = the System `Name`
///   4 Bonds            Undefined(66), write         three `address(6) + name(16)` entries
///   5 Window           Uint32, write (Trigger)      ms left in the connectable window, 0 = closed
///
/// The block is SIX fields and nothing else (owner ruling 2026-10-29): `Action`, `Bond count`, the
/// keyed `Bond 0..2` rows and the `Last peer`/`Last air`/`Last link` read-out are gone. What they
/// used to say lives in the `Bonds` array (which slots hold a bond, and each peer's label) and in
/// the `Window` field, which is now a command as well as a read-out.
///
/// Three properties shape everything the panel does, and all come from the firmware
/// (`Devices/Tamu_v2.0A/BleSettingsGlue.h`, `Devices/Tamu_v2.0A/AppBLE.h`):
///
///  - **Every write to this block is refused unless the frame arrived over USB.** A phone that
///    could rotate the key, delete the bonds, rename the device or switch its radio off would defeat
///    the passkey it is protecting. So over a BLE link the panel is a read-out plus an explanation
///    of why the controls cannot work — and a refused write is a normal `FLAG_FAIL` reply, which
///    the app surfaces as a sentence rather than leaving a control that silently does nothing.
///  - **`Window` reads the milliseconds LEFT and a write sets the deadline** (0 closes it). The old
///    `Action` enum carried the same command; it is gone, so opening a window is one field write.
///  - **`Display name` IS the advertised name.** A write re-advertises in the same loop pass, so
///    there is no "next advertising start" — that could mean never on a device that advertises
///    whenever it is idle, which is why the field and the air cannot disagree any more.
///
/// Advertising has one condition: the radio is up and enabled and no session is up (E64 ruling C,
/// `BleAdvWanted()`). It never depends on the bond store or on a window, so the block's `Window` of
/// 0 means "no pairing window", not "nothing on the air". What the window and the store do decide
/// is **who may connect**: a bonded phone always, and any other phone while a window is open or
/// while the store is still empty - an empty store is exactly how the first bond gets made. The gate
/// is the device's own session check, never the controller's accept list (E66: nothing sets an
/// advertising filter policy and the bond store is not mirrored into the controller's white list,
/// because that list is matched against an address a privacy-enabled phone never connects from). So
/// a stranger's CONNECT_REQ is answered and the link is refused when it cannot prove a bond - from
/// that phone, a failed pairing rather than a silent absence. An empty store has nothing to protect
/// and admits anyone; it also opens its own five-minute window once per boot, and again after an
/// explicit forget emptied it. Neither shape of "it does not connect" is a broken device
/// ([bleLinkGuidance]).
library;

import 'types.dart';

/// Field numbers of the `BLE Settings` block, in table order (firmware `Blocks/BleSettings.h`,
/// `BLE_SET_FIELD_*`) — six fields, nothing else.
const int bleFieldEnabled = 0;
const int bleFieldPasskey = 1;
const int bleFieldMode = 2;
const int bleFieldDisplayName = 3;
const int bleFieldBonds = 4;
const int bleFieldWindow = 5;

/// How many fields the block carries (0..5).
const int bleFieldCount = 6;

/// The block type the firmware assigns to `BLE Settings` (`BlockType::BleSettings`).
const int bleSettingsBlockTypeValue = 0x0B;

/// Bond slots the block exposes — one per `CONFIG_BT_NIMBLE_MAX_BONDS` (three today). The
/// firmware static-asserts the two numbers together, and the `Bonds` value's declared size is
/// [bleBondsLength], which is exactly `slots × entry`.
const int bleBondSlots = 3;

/// One entry of the `Bonds` array: the peer's identity address (six bytes, **printed order**) and
/// its label (sixteen bytes, NUL-padded). `BLE_BOND_ADDR_LEN` / `BLE_BOND_NAME_LEN` in the header.
const int bleBondAddressLength = 6;
const int bleBondNameLength = 16;

/// `address + name` — `BLE_BOND_ENTRY_LEN`.
const int bleBondEntryLength = bleBondAddressLength + bleBondNameLength;

/// The `Bonds` value's declared size: three entries, `BLE_BOND_ARRAY_LEN`. The device requires the
/// whole array in one write (the register path demands a non-Text field's declared size), so this
/// is also the length of every write the panel sends.
const int bleBondsLength = bleBondSlots * bleBondEntryLength;

/// The passkey's width: six ASCII digits, exactly (the device refuses any other length).
const int blePasskeyLength = 6;

/// The advertised-name field's width (`BLE_DISPLAY_NAME_LEN`).
const int bleDisplayNameWidth = 16;

/// The passkey that means "no passkey": pairing falls back to Just Works.
const String blePasskeyNone = '000000';

/// `Mode` values, exactly as the firmware numbers them.
const int bleModeJustWorks = 0;
const int bleModePasskey = 1;

const Map<int, String> bleModeLabels = {
  bleModeJustWorks: 'Just Works',
  bleModePasskey: 'Passkey',
};

/// The label for a `Mode` value, `Mode N` for one this build does not know.
String bleModeLabel(int mode) => bleModeLabels[mode] ?? 'Mode $mode';

/// The longest window a write may ask for. The device's countdown compares signed, so a deadline
/// further away than `INT32_MAX` ms would read as already past; `AppBLESetWindow` refuses such a
/// write instead of clamping it.
const int bleWindowMaximumMs = 0x7FFFFFFF;

/// Why [ms] cannot be written to `Window`, or null when it can.
///
/// `0` always closes (the device accepts it with the radio down), a non-zero value needs the radio
/// switched on, and nothing above [bleWindowMaximumMs] is accepted. The dialog checks this before
/// sending so a refusal is not the first the user hears of it.
String? bleWindowProblem(int ms, {required bool enabled}) {
  if (ms < 0) return 'a window length cannot be negative';
  if (ms > bleWindowMaximumMs) {
    return 'the window must be at most ${bleWindowMaximumMs ~/ 1000} seconds';
  }
  if (ms > 0 && !enabled) {
    return 'the radio is switched off, so no window can open';
  }
  return null;
}

/// One slot of the `Bonds` array: a peer's identity address and the label kept beside it.
///
/// The device publishes the address in the order it is printed (`AA:BB:CC:DD:EE:FF`), not in
/// NimBLE's least-significant-first storage order — the firmware reverses it for us
/// (`Blocks/BleSettings.h`, "The Bonds array"). An ALL-ZERO address is the device's own empty-slot
/// marker; a real identity address is never all zero.
class BleBond {
  const BleBond({required this.address, this.name = ''});

  /// The six address bytes, in printed order.
  final List<int> address;

  /// The label stored beside the address (`BleBondLabelStore.h`). The NimBLE store keeps no peer
  /// name (Issues.md E60), so this is the app's own label — and it is what a rename writes.
  final String name;

  /// True for an all-zero address: no bond in this slot.
  bool get isEmpty => address.every((byte) => byte == 0);

  /// The address as `AA:BB:CC:DD:EE:FF`, or an empty string for an empty slot.
  String get addressText => isEmpty ? '' : address.map(_bleHexByte).join(':');

  /// True when [address] is this entry's address, byte for byte.
  bool holds(List<int> other) =>
      address.length == other.length &&
      [
        for (var i = 0; i < address.length; i++) address[i] == other[i],
      ].every((same) => same);

  @override
  String toString() => 'BleBond(${isEmpty ? '<empty>' : addressText}, "$name")';
}

String _bleHexByte(int byte) =>
    (byte & 0xFF).toRadixString(16).padLeft(2, '0').toUpperCase();

/// Three empty slots — the reading of a `Bonds` field that answered with zeros, and the default of
/// a snapshot whose device did not answer it at all.
const BleBond bleEmptyBond = BleBond(address: [0, 0, 0, 0, 0, 0]);
const List<BleBond> bleEmptyBonds = [bleEmptyBond, bleEmptyBond, bleEmptyBond];

/// Decodes the `Bonds` value into its [bleBondSlots] slots, empties included.
///
/// The value is `ValueInfo.Size` bytes ([bleBondsLength] = 66); a read pads its reply to a
/// four-byte boundary, so the bytes handed back can be 68 long and only the first 66 are entries.
/// A slot past the end of a short value reads as empty, so a truncated reply cannot shift an
/// address into a name.
List<BleBond> decodeBleBonds(List<int> value) {
  List<int> slice(int start, int length) => [
    for (var i = 0; i < length; i++)
      start + i < value.length ? value[start + i] & 0xFF : 0,
  ];
  return [
    for (var slot = 0; slot < bleBondSlots; slot++)
      BleBond(
        address: slice(slot * bleBondEntryLength, bleBondAddressLength),
        name: decodePaddedString(
          slice(
            slot * bleBondEntryLength + bleBondAddressLength,
            bleBondNameLength,
          ),
        ),
      ),
  ];
}

/// The wire value for [bonds]: always [bleBondsLength] bytes, unknown slots padded with zeros.
///
/// The device requires exactly the declared size for this field, so a write built here is always
/// the whole array — one write describes every slot, and the reply describes all of them.
List<int> encodeBleBonds(List<BleBond> bonds) {
  final out = List<int>.filled(bleBondsLength, 0);
  for (var slot = 0; slot < bleBondSlots && slot < bonds.length; slot++) {
    final base = slot * bleBondEntryLength;
    final bond = bonds[slot];
    for (var i = 0; i < bleBondAddressLength && i < bond.address.length; i++) {
      out[base + i] = bond.address[i] & 0xFF;
    }
    final name = bond.name.codeUnits;
    for (var i = 0; i < bleBondNameLength && i < name.length; i++) {
      out[base + bleBondAddressLength + i] = name[i] & 0xFF;
    }
  }
  return out;
}

/// The array to send when [slot]'s label is renamed: every other entry untouched.
List<int> bleBondsRenamed(List<BleBond> bonds, int slot, String name) =>
    encodeBleBonds([
      for (var i = 0; i < bonds.length; i++)
        i == slot ? BleBond(address: bonds[i].address, name: name) : bonds[i],
    ]);

/// The array to send when [slot]'s address is cleared, which deletes that bond: the address is
/// zeroed and the label goes with it. Every other entry is untouched, so a device that holds more
/// than one bond keeps the rest.
///
/// Zeroing the last bond empties the store, and the firmware answers that with the one
/// implementation of "every bond is gone" (`AppBLEForgetAllBonds`): the store is wiped and the
/// automatic five-minute pairing window is owed.
List<int> bleBondsCleared(List<BleBond> bonds, int slot) => encodeBleBonds([
  for (var i = 0; i < bonds.length; i++) i == slot ? bleEmptyBond : bonds[i],
]);

/// What is stated about a peer that was never paired, wherever the panel has to explain the
/// missing slot: the array is a VIEW of the bond store, so a peer appears in it by pairing and by
/// nothing else.
const String bleBondCannotAdd =
    'A peer cannot be added by hand: a bond is created by pairing, never by writing this list.';

/// Why the device refused a `Bonds` write, in one sentence.
///
/// The device decides the WHOLE frame before applying any of it: a non-zero address that is not
/// the one its store holds in that slot refuses the write outright, and nothing is applied - names
/// included. A bond cannot be created by a register write at all (the LTK/IRK live in the bond
/// store, never in the register file), so an entry the panel invented is a statement about
/// pairing rather than a failure to retry. The other shape of a refusal is a list the device has
/// moved on from: entries are matched slot by slot, so a bond that was evicted (or added) since
/// the panel read the array makes the write stale rather than wrong.
String bleBondsRefusal({
  required bool overUsb,
  required List<int> sent,
  required List<BleBond> held,
}) {
  if (!overUsb) return bleWriteRefusal(overUsb: false);
  final sentBonds = decodeBleBonds(sent);
  final invented = [
    for (var slot = 0; slot < sentBonds.length && slot < held.length; slot++)
      if (!sentBonds[slot].isEmpty &&
          !held[slot].holds(sentBonds[slot].address))
        sentBonds[slot].addressText,
  ];
  if (invented.isNotEmpty) {
    return '$bleBondCannotAdd Pair the phone from its Bluetooth settings while a pairing window '
        'is open, and it appears here.';
  }
  return 'The device refused the change: its bond list is not the one this panel read. Read it '
      'again and retry.';
}

/// Whether the device's block enumeration carries the BLE Settings block. It is compiled into the
/// core only, so a node's absence of it is the normal case rather than an error.
bool hasBleSettingsBlock(Iterable<({int type, int maxInstance})> types) =>
    types.any((t) => t.type == bleSettingsBlockTypeValue);

/// One field read as the device answered it, or null when it did not answer.
typedef BleFieldRead = ({ValueInfo meta, List<int> value})?;

/// One reading of the block's fields, as the device reported them.
///
/// Nothing here is inferred except the two derived flags the block states itself: the mode is the
/// device's own `Mode` field (never recomputed from the passkey text), and the window is the
/// device's own countdown in milliseconds.
class BleSecuritySnapshot {
  const BleSecuritySnapshot({
    this.passkey = blePasskeyNone,
    this.mode = bleModeJustWorks,
    this.bonds = bleEmptyBonds,
    this.windowMs = 0,
    this.enabled = false,
    this.displayName = '',
    this.answered = const <int>{},
  });

  /// Builds the snapshot from the field reads, keyed by field number (every field has key 0 — the
  /// keyed `Bond` rows are gone, the array carries all three slots).
  ///
  /// A field the device did not answer keeps its empty default here (an unanswered passkey stays
  /// `''`, not `000000`) and is NOT recorded in [answered]. The defaults alone cannot be told
  /// apart from a device really reporting "no passkey, closed, off", so the panel asks
  /// [answeredField] before printing a field and shows `-` for one it never heard about.
  factory BleSecuritySnapshot.fromReads(Map<int, BleFieldRead> fields) {
    List<int> bytesOf(int field) => fields[field]?.value ?? const <int>[];
    final window = bytesOf(bleFieldWindow);
    return BleSecuritySnapshot(
      passkey: decodePaddedString(bytesOf(bleFieldPasskey)),
      mode: bytesOf(bleFieldMode).isEmpty
          ? bleModeJustWorks
          : bytesOf(bleFieldMode)[0],
      bonds: fields[bleFieldBonds] == null
          ? bleEmptyBonds
          : decodeBleBonds(bytesOf(bleFieldBonds)),
      windowMs: window.length >= 4 ? uint32FromBytes(window) : 0,
      enabled: bytesOf(bleFieldEnabled).any((b) => b != 0),
      displayName: decodePaddedString(bytesOf(bleFieldDisplayName)),
      answered: {
        for (final entry in fields.entries)
          if (entry.value != null) entry.key,
      },
    );
  }

  /// The code the device holds, six digits (`000000` when there is none). It reads back
  /// deliberately — it is the user's own code, not a password the device hides.
  final String passkey;

  /// The device's effective policy (`Mode`), derived from the passkey on the device.
  final int mode;

  /// The `Bonds` array, one entry per slot and in the device's own order (the bond store's, oldest
  /// first). Empty slots are kept: the panel draws a row for each, which is how "there is room for
  /// another phone, and it gets there by pairing" can be said at all.
  final List<BleBond> bonds;

  /// Milliseconds left in the connectable window; 0 means closed.
  final int windowMs;

  /// Whether the block wants the radio used at all.
  final bool enabled;

  /// The name the user set for the advertisement; empty means the System `Name` is used. This IS
  /// the advertised name (the block has no second read-out): a write re-advertises at once.
  final String displayName;

  /// The field numbers the device actually answered in this reading, from
  /// [BleSecuritySnapshot.fromReads]. Empty for a snapshot that was stated rather than read (a
  /// hand-built one), which is the same as "nothing is missing": there is nothing to be unsure
  /// about when the caller names every field itself.
  final Set<int> answered;

  /// True when the device answered [field] in this reading, i.e. the value beside it came from the
  /// device rather than from the empty default.
  bool answeredField(int field) => answered.isEmpty || answered.contains(field);

  /// True when the reading missed one of the fields the state is derived from, so the panel cannot
  /// say what the radio is doing ([BleSecurityCondition.incomplete]).
  bool get missingStateField =>
      answered.isNotEmpty &&
      !(answered.contains(bleFieldEnabled) &&
          answered.contains(bleFieldWindow) &&
          answered.contains(bleFieldBonds));

  /// True when a passkey protects pairing (the device's own `Mode` says so).
  bool get passkeySet => mode == bleModePasskey;

  /// True while the device is discoverable and connectable.
  bool get windowOpen => windowMs > 0;

  /// How many slots hold a bond. The array is the count now — there is no separate `Bond count`
  /// field, and an all-zero address is the device's own empty-slot marker.
  int get bondCount => bonds.where((bond) => !bond.isEmpty).length;

  /// How many slots are free.
  int get emptyBondSlots => bonds.where((bond) => bond.isEmpty).length;

  /// True when the radio is on but nothing else is known: no passkey, no bonds, no window and no
  /// name. That is a block a build reports without driving a radio, and the panel says so instead
  /// of drawing a working link.
  bool get isEmpty =>
      !enabled &&
      !passkeySet &&
      bondCount == 0 &&
      !windowOpen &&
      displayName.isEmpty;

  /// The state the panel reports for this reading.
  BleSecurityCondition get condition => bleSecurityCondition(this);

  @override
  String toString() =>
      'BleSecuritySnapshot(mode: ${bleModeLabel(mode)}, bonds: $bondCount, '
      'window: ${bleWindowText(windowMs)}, enabled: $enabled, '
      'displayName: "$displayName")';
}

/// What the panel says about a reading.
enum BleSecurityCondition {
  /// `Enabled` is false: the device does not use its radio at all.
  radioOff,

  /// A window is open now, so the device is discoverable.
  windowOpen,

  /// The window is closed and the device holds bonds: its bonded phones still connect at any
  /// time, and only a new pairing needs a window (which only USB can open).
  closedBonded,

  /// The window is closed and the store is empty: the device holds no bonds, so any phone may
  /// connect and pair — that is how the first bond is made. It is on the air like any other
  /// peripheral, and it also opens a five-minute window by itself at boot.
  closedNoBonds,

  /// A reading of nothing at all ([BleSecuritySnapshot.isEmpty]): the block exists but describes
  /// no radio.
  noRadio,

  /// The device answered only part of the block, so the fields the state is derived from
  /// (`Enabled`, `Window`, `Bond count`) are not all there: what the radio is doing cannot be
  /// said, and the panel says that instead of drawing a state from defaults.
  incomplete,
}

/// Classifies one reading. A reading that missed one of the fields the state is derived from has
/// no state to classify — the defaults would read as "radio off, no window, no bonds", which is a
/// claim about the device rather than a fact about the read. Otherwise `Enabled` decides first (a
/// switched-off radio has no window whatever the other fields say), then the window, then what the
/// store holds.
BleSecurityCondition bleSecurityCondition(BleSecuritySnapshot snapshot) {
  if (snapshot.missingStateField) return BleSecurityCondition.incomplete;
  if (!snapshot.enabled) {
    return snapshot.isEmpty
        ? BleSecurityCondition.noRadio
        : BleSecurityCondition.radioOff;
  }
  if (snapshot.windowOpen) return BleSecurityCondition.windowOpen;
  return snapshot.bondCount == 0
      ? BleSecurityCondition.closedNoBonds
      : BleSecurityCondition.closedBonded;
}

/// The headline the panel shows for [condition].
String bleSecurityHeadline(BleSecurityCondition condition) =>
    switch (condition) {
      BleSecurityCondition.radioOff => 'Bluetooth is off',
      BleSecurityCondition.windowOpen => 'Discoverable now',
      BleSecurityCondition.closedBonded => 'Bonded phones only',
      BleSecurityCondition.closedNoBonds => 'No bonds: any phone may pair',
      BleSecurityCondition.noRadio => 'No radio described by this block',
      BleSecurityCondition.incomplete =>
        'The device did not answer every field',
    };

/// The line under the headline: why the device is in that state, in the device's own terms.
String bleSecurityDetail(
  BleSecurityCondition condition,
  BleSecuritySnapshot snapshot,
) => switch (condition) {
  BleSecurityCondition.radioOff =>
    'The device is not using its radio at all: nothing is advertised and no connection is '
        'accepted. Switch it back on over USB.',
  BleSecurityCondition.windowOpen =>
    snapshot.bondCount == 0
        ? 'The device holds no bonds, so it is waiting for its first phone to pair. '
              'Any device may pair until the window closes.'
        : 'Any device may pair until the window closes.',
  // E64 ruling C: advertising does not depend on the window, so a closed window is about
  // strangers, never about the phone that already paired.
  BleSecurityCondition.closedBonded =>
    'Bonded devices can reconnect at any time. New pairings need a window, which only USB '
        'can open.',
  BleSecurityCondition.closedNoBonds =>
    'The device holds no bonds, so any phone may pair with it: pair from your phone\'s '
        'Bluetooth settings. It opens a five-minute window by itself when it boots, and a '
        'window can also be opened over USB at any time.',
  BleSecurityCondition.noRadio =>
    'The block reports no passkey, no bonds, no window and no name, which is a block a build '
        'reports without driving a radio.',
  BleSecurityCondition.incomplete =>
    'The device answered only part of its Bluetooth settings, so what its radio is doing right '
        'now cannot be said. The rows it skipped show "-": read the block again to fill them in.',
};

/// A countdown as `m:ss`, rounding up so a sub-second remainder still reads as a second.
String bleWindowCountdown(int ms) {
  final total = (ms + 999) ~/ 1000;
  return '${total ~/ 60}:${(total % 60).toString().padLeft(2, '0')}';
}

/// The `Window` field in words: `closed`, or the countdown left.
String bleWindowText(int ms) =>
    ms <= 0 ? 'closed' : '${bleWindowCountdown(ms)} left';

/// The line under the bond list: what the array means as a whole, or null when the device did not
/// answer it (the panel then prints `-` and makes no claim about what is stored).
///
/// The distinction that matters is between "no bond" and "not read": an empty store is a device
/// that admits any phone, which is the state a first pairing is made in, never a silent device.
String? bleBondsDetail(BleSecuritySnapshot snapshot) {
  if (!snapshot.answeredField(bleFieldBonds)) return null;
  final bonds = snapshot.bondCount;
  if (bonds == 0) {
    return 'No phone is paired. Any phone may pair, and it appears here once it has: '
        '$bleBondCannotAdd';
  }
  final free = snapshot.emptyBondSlots;
  return free == 0
      ? '$bonds phones are paired and the list is full. Pairing another one replaces the oldest: '
            'the bond store holds ${snapshot.bonds.length} at a time.'
      : '$bonds of ${snapshot.bonds.length} slots hold a bond. A new phone pairs into a free one '
            'while a pairing window is open - $bleBondCannotAdd';
}

/// True when [text] is exactly six ASCII digits — the only shape the device accepts for the
/// passkey (`OnBlePasskeyWrite` refuses anything else).
bool blePasskeyIsValid(String text) {
  if (text.length != blePasskeyLength) return false;
  return text.codeUnits.every((c) => c >= 0x30 && c <= 0x39);
}

/// Why a control on the panel cannot work over this link, in one sentence — or null when it can.
///
/// The device refuses every write to this block that did not arrive over USB
/// (`BleBlockWritable`), so the panel says it before the user fills a form rather than after a
/// rejected write.
String? bleWriteRestriction({required bool overUsb}) => overUsb
    ? null
    : 'Bluetooth settings can only be changed over a USB link to the device. A phone cannot '
          'change the key, wipe the bonds, rename the device or switch its radio off.';

/// Why a write the panel sent was refused, as a sentence the user can act on.
///
/// A refusal is the normal `FLAG_FAIL` reply: the device answered, it just said no. The two cases
/// are told apart because the fix differs — a link that cannot write at all, and a value the
/// device would not accept.
String bleWriteRefusal({required bool overUsb, String? valueHint}) {
  if (!overUsb) {
    return 'The device refused the change: this block can only be written over USB, and this '
        'app is connected over Bluetooth.';
  }
  return valueHint == null
      ? 'The device refused the change.'
      : 'The device refused the change: $valueHint';
}

/// How long the firmware lets a new link stay unsecured before it drops it, from
/// `firmware/src/Blocks/BleSecurityPolicy.h` (`BleSecureBudgetJustWorksMs` /
/// `BleSecureBudgetPasskeyMs`, pinned against that header by
/// `test/firmware_contract_test.dart`).
///
/// The budget is policy-shaped because the phone's half is: Just Works is one tap — or nothing at
/// all — and is over in well under a second, while a passkey makes the phone show a dialog and a
/// person type six digits. A drop is judged against the LARGER figure: after a link is gone the app
/// cannot tell which policy was in force (there is no link to read `Mode` over, and the two cases
/// look identical from here), and judging against the Just Works figure would miss every drop in
/// the 5-30 s band — exactly the band a passkey pairing lives in, and the case where a passkey
/// change has just revoked the phone's bond.
const int bleSecureBudgetJustWorksMs = 5000;
const int bleSecureBudgetPasskeyMs = 30000;

/// Why a BLE attempt did not produce a session.
enum BleLinkSymptom {
  /// The pairing or its encryption failed: the phone's stored bond is stale (the device no
  /// longer holds it) or the device now asks for a code the phone does not have.
  securityRefused,

  /// The link came up and died before the firmware's securing budget ran out
  /// ([bleSecureBudgetPasskeyMs], the larger of the two), which is the stale-bond case seen from
  /// the app's side.
  droppedDuringSecuring,

  /// The device never answered: it is off, out of range, or its radio is switched off. A phone
  /// the device holds no bond for is not silent in the same way — the device answers its connect
  /// request and refuses the link once it cannot prove a bond, so the refusal normally reaches the
  /// phone as a failed pairing ([securityRefused]). Some stacks report a refuse-then-drop as a
  /// bare timeout anyway; the device's own `Bonds` list (read over USB) says whether it holds a
  /// bond for that phone at all.
  unreachable,

  /// Anything else. The panel then lists both causes instead of guessing between them.
  unknown,
}

/// What the app tells the user after a failed BLE attempt, in plain words.
class BleLinkGuidance {
  const BleLinkGuidance({
    required this.symptom,
    required this.headline,
    required this.detail,
    required this.steps,
    this.likelyStaleBond = false,
  });

  final BleLinkSymptom symptom;

  /// The one-line statement of what went wrong.
  final String headline;

  /// Why it happened, in plain words, without blaming the device.
  final String detail;

  /// What the user should do, in order.
  final List<String> steps;

  /// True when the guidance names the stale bond as the cause (the card's case).
  final bool likelyStaleBond;
}

/// Error-text fragments that mean the pairing or the encryption failed rather than the link being
/// out of reach. Lowercase substrings, because every platform words these differently: Android
/// reports `GATT_INSUFFICIENT_AUTHENTICATION` (status 5) and `GATT_INSUFFICIENT_ENCRYPTION`
/// (status 15), BlueZ reports an authentication/key failure, and the ATT layer's own words are
/// "insufficient authentication/encryption".
const List<String> _securityFailureTokens = [
  'insufficient authentication',
  'insufficient encryption',
  'insufficient_authentication',
  'insufficient_encryption',
  'authentication failed',
  'authentication failure',
  'authentication canceled',
  'authentication cancelled',
  'authentication timeout',
  'auth fail',
  'encryption failed',
  'encryption fail',
  'encrypt failed',
  'bond',
  'pairing',
  'passkey',
  'key missing',
  'no key',
  'status 5',
  'status 15',
  'status_insufficient',
];

/// Error-text fragments that mean "nothing answered" rather than "the answer was refused".
const List<String> _unreachableTokens = [
  'timeout',
  'timed out',
  'not found',
  'device not found',
  'no device',
  'unreachable',
  'no route',
  'not advertising',
  'host is down',
  'refused',
];

/// Whether the platform's error text is the pairing or the encryption failing.
///
/// The two halves are exposed because a connect attempt has to place a failure the app never
/// watched die: [classifyConnectFailure] in `connection.dart` reads these to tell "the device
/// answered and refused the phone" from "nothing answered at all".
bool bleErrorLooksLikeSecurity(String? errorText) {
  final text = (errorText ?? '').toLowerCase();
  return text.isNotEmpty &&
      _securityFailureTokens.any((token) => text.contains(token));
}

/// Whether the platform's error text is "nothing answered" rather than "the answer was refused".
bool bleErrorLooksUnreachable(String? errorText) {
  final text = (errorText ?? '').toLowerCase();
  return text.isNotEmpty &&
      _unreachableTokens.any((token) => text.contains(token));
}

/// Classifies one failed BLE attempt.
///
/// The app cannot read the block after a failure (there is no link), and there is no per-link
/// "bonded" flag anywhere in the protocol, so this is a reading of the evidence the app does
/// have: the error text the platform produced, and whether a link came up and then died. Both are
/// signals, not proof, which is why the wording of [likelyStaleBond] guidance names the bond as
/// the likely cause and gives the window case too when the evidence is ambiguous.
BleLinkGuidance bleLinkGuidance({
  String? errorText,
  bool droppedWhileSecuring = false,
}) {
  final symptom = _classifyBleFailure(
    errorText: errorText,
    droppedWhileSecuring: droppedWhileSecuring,
  );
  switch (symptom) {
    case BleLinkSymptom.securityRefused:
    case BleLinkSymptom.droppedDuringSecuring:
      return const BleLinkGuidance(
        symptom: BleLinkSymptom.securityRefused,
        headline: 'This phone is no longer paired with the device',
        detail:
            'The device no longer holds a bond for this phone, so it refuses the link. That is '
            'the normal result of a firmware update, of deleting the bond in the device\'s own bond '
            'list, or of changing the '
            'passkey - the device is working, it just has to pair with this phone again.',
        steps: [
          "Open your phone's Bluetooth settings and find this device in the paired list.",
          'Choose "Forget" (or "Unpair") for it.',
          'Come back here and connect again. Pairing again is a new pairing, so it needs a '
              'pairing window: open one from the device\'s Bluetooth panel over USB, and on a '
              'device whose bond list is now empty a power cycle opens a five-minute one by '
              'itself.',
        ],
        likelyStaleBond: true,
      );

    case BleLinkSymptom.unreachable:
      return const BleLinkGuidance(
        symptom: BleLinkSymptom.unreachable,
        headline: 'The device did not answer this phone',
        detail:
            'The device answers a connect request from any phone and refuses the link when that '
            'phone cannot prove a bond - a phone it holds no bond for sees the pairing fail, not '
            'an unanswered request. An attempt that got no answer at all therefore points at the '
            'device: it is off, out of range, or has its radio switched off. A device that holds '
            'no bonds admits any phone, so an empty store is never why it cannot be found.',
        steps: [
          'Make sure the device is powered and near this phone.',
          'If this app can reach the device over USB, its Bluetooth panel says whether it holds '
              'bonds and whether a pairing window is open.',
          'A phone the device holds no bond for needs a pairing window: open one from that panel '
              '(only USB can) and connect again within the time it shows.',
          'A device whose bond list is empty admits any phone, and opens a five-minute window by '
              'itself on every boot, so pairing still failing there means the device is off, out '
              'of range, or its radio is switched off.',
        ],
      );

    case BleLinkSymptom.unknown:
      return const BleLinkGuidance(
        symptom: BleLinkSymptom.unknown,
        headline: 'Connection failed',
        detail:
            'A Bluetooth connection can fail in two ways that look alike from the phone: the '
            'device holds no bond for this phone and refuses its link once it connects - the '
            'request is answered, but the phone then cannot prove a bond - or the device is off, '
            'out of range or has its radio switched off. A device that holds no bonds admits '
            'any phone, so it is not a third case.',
        steps: [
          'If this app can reach the device over USB, its Bluetooth panel says which of the two '
              'it is.',
          'Open a pairing window from that panel (only USB can) and connect again within the '
              'time it shows: pairing is the only way in for a phone the device holds no bond '
              'for.',
          "Otherwise open your phone's Bluetooth settings, choose \"Forget\" for this device and "
              'connect again - that clears a bond the device no longer has. It is the normal step '
              'after a firmware update, and pairing again then needs a window too.',
        ],
      );
  }
}

/// The symptom alone, for callers that want to branch without building the copy.
BleLinkSymptom classifyBleLinkFailure({
  String? errorText,
  bool droppedWhileSecuring = false,
}) => _classifyBleFailure(
  errorText: errorText,
  droppedWhileSecuring: droppedWhileSecuring,
);

BleLinkSymptom _classifyBleFailure({
  String? errorText,
  required bool droppedWhileSecuring,
}) {
  if (bleErrorLooksLikeSecurity(errorText)) {
    return BleLinkSymptom.securityRefused;
  }
  if (droppedWhileSecuring) {
    // A link that came up and died before it was ever usable is the shape of the device's
    // secure-link deadline, not of a device out of reach.
    return BleLinkSymptom.droppedDuringSecuring;
  }
  if (bleErrorLooksUnreachable(errorText)) return BleLinkSymptom.unreachable;
  return BleLinkSymptom.unknown;
}
