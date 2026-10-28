/// The `BLE Settings` block (BlockType `0x0B`, core only): the BLE link's settings and state as
/// register fields, so the panel in the app reads and writes them through the same register path
/// as every other block instead of a side-channel command set.
///
/// The block's table order is fixed here because the panel, the block registry and the firmware
/// contract test all address the fields (and the Bond keys) by number:
///
///   0 Passkey          Text(6), write (Trigger)  6 ASCII digits, "000000" = Just Works
///   1 Mode             Enum, read-only           0 Just Works, 1 Passkey, derived from the passkey
///   2 Bond count       Index, read-only          entries in the device's bond store
///   3 Bond[0..2]       Text(20), read-only       "AA:BB:CC:DD:EE:FF", OLDEST FIRST, address only
///   4 Window           Uint32, read-only         ms left in the connectable window, 0 = closed
///   5 Action           Enum, write (Trigger)     0 open the pairing window, 1 forget all bonds
///   6 Enabled          Bool, persistent+write    false = the radio is not used at all
///   7 Display name     Text(16), persistent+write advertised name, empty = the System `Name`
///   8 Advertised name  Text(16), read-only       the name actually on the air
///   9 Last peer        Text(20), read-only       identity the last admission used (E66), "AA:BB:CC:DD:EE:FF/N"
///  10 Last air         Text(20), read-only       address the same link came in on, "AA:BB:CC:DD:EE:FF/N"
///  11 Last link        Enum, read-only           how the last judged link ended ([bleVerdictLabels])
///
/// Fields 9 and 10 are read as a PAIR, and the pair is the witness: they differ when the controller
/// resolved a rotating private address to the identity in `Last peer`, which is the reading E66
/// exists for ([BleSecuritySnapshot.lastAddressResolved]). **Equal addresses are the correct
/// reading** for a peer that connects from its own public or static address — this rig's host does
/// exactly that — and are never a sign that the read-out failed. The `/N` suffix is information
/// only (NimBLE normalises the controller's resolved types 2 and 3 to 0 and 1, so it never reads 2
/// or 3), and it is never part of the comparison.
///
/// Two properties shape everything the panel does, and both come from the firmware
/// (`Devices/Tamu_v2.0A/BleSettingsGlue.h`, `Devices/Tamu_v2.0A/AppBLE.h`):
///
///  - **Every write to this block is refused unless the frame arrived over USB.** A phone that
///    could rotate the key, wipe the bonds, rename the device or switch its radio off would defeat
///    the passkey it is protecting. So over a BLE link the panel is a read-out plus an explanation
///    of why the controls cannot work — and a refused write is a normal `FLAG_FAIL` reply, which
///    the app surfaces as a sentence rather than leaving a control that silently does nothing.
///  - **Advertising has one condition: the radio is up and enabled and no session is up** (E64
///    ruling C, `BleAdvWanted()`). It never depends on the bond store or on a window, so the
///    block's `Window` of 0 means "no pairing window", not "nothing on the air". What the window
///    and the store do decide is **who may connect**: a bonded phone always, and any other phone
///    while a window is open or while the store is still empty - an empty store is exactly how the
///    first bond gets made. The gate is the device's own session check, never the controller's
///    accept list (E66: nothing sets an advertising filter policy and the bond store is not
///    mirrored into the controller's white list, because that list is matched against an address a
///    privacy-enabled phone never connects from). So a stranger's CONNECT_REQ is answered and the
///    link is refused when it cannot prove a bond - from that phone, a failed pairing rather than
///    a silent absence. An empty store has nothing to protect and admits anyone; it also opens its
///    own five-minute window once per boot and again after `Action = 1`. Only USB can open a window
///    (`Action = 0`). Neither shape of "it does not connect" is a broken device
///    ([bleLinkGuidance]).
library;

import 'types.dart';

/// Field numbers of the `BLE Settings` block, in table order (firmware `Blocks/BleSettings.h`,
/// `BLE_SET_FIELD_*`).
const int bleFieldPasskey = 0;
const int bleFieldMode = 1;
const int bleFieldBondCount = 2;
const int bleFieldBond = 3;
const int bleFieldWindow = 4;
const int bleFieldAction = 5;
const int bleFieldEnabled = 6;
const int bleFieldDisplayName = 7;
const int bleFieldAdvertisedName = 8;
const int bleFieldLastPeer = 9;
const int bleFieldLastAir = 10;
const int bleFieldLastLink = 11;

/// How many fields the block carries (0..11).
const int bleFieldCount = 12;

/// The block type the firmware assigns to `BLE Settings` (`BlockType::BleSettings`).
const int bleSettingsBlockTypeValue = 0x0B;

/// Bond slots the block exposes — one per `CONFIG_BT_NIMBLE_MAX_BONDS` (three today). The
/// firmware static-asserts the two numbers together.
const int bleBondSlots = 3;

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

/// `Action` values: what a write to field 5 asks the device to do.
const int bleActionOpenWindow = 0;
const int bleActionForgetBonds = 1;

const Map<int, String> bleActionLabels = {
  bleActionOpenWindow: 'Open pairing window',
  bleActionForgetBonds: 'Forget all bonds',
};

/// `Last link` values: the verdict of the last judged link, exactly as the firmware numbers
/// `BleLinkVerdict` (`firmware/src/Blocks/BleAdmissionPolicy.h`). E66 added the field because the
/// log cannot answer the question it settles: while a host holds the link the device's log is
/// dropped (Issues.md E67), and a connect happens exactly then.
///
/// The distinction that matters is between a stranger being refused and a bonded peer whose
/// address was not recognised — two links that look identical from the phone.
const int bleVerdictNone = 0;
const int bleVerdictPending = 1;
const int bleVerdictAdmitted = 2;
const int bleVerdictRefusedNotBonded = 3;
const int bleVerdictRefusedOutsideWindow = 4;
const int bleVerdictRefusedUnsecured = 5;
const int bleVerdictRefusedBusy = 6;

const Map<int, String> bleVerdictLabels = {
  bleVerdictNone: 'No link judged',
  bleVerdictPending: 'Not judged yet',
  bleVerdictAdmitted: 'Admitted',
  bleVerdictRefusedNotBonded: 'Refused: not bonded',
  bleVerdictRefusedOutsideWindow: 'Refused: not in a window',
  bleVerdictRefusedUnsecured: 'Refused: not secured',
  bleVerdictRefusedBusy: 'Refused: radio busy',
};

/// The label for a `Last link` value, `Verdict N` for one this build does not know.
String bleVerdictLabel(int verdict) =>
    bleVerdictLabels[verdict] ?? 'Verdict $verdict';

/// What a `Last link` verdict means, in one sentence. The two refusals are spelled out because
/// telling them apart is the whole reason E66 added the field: a stranger refused at connect and a
/// bonded phone whose address the device did not recognise look identical from the phone.
String bleVerdictDetail(int verdict) => switch (verdict) {
  bleVerdictNone => 'No connect has been judged since the device booted.',
  bleVerdictPending =>
    'A link came up and the device has not finished judging it yet.',
  bleVerdictAdmitted => 'A bonded phone connected and the session was secured.',
  bleVerdictRefusedNotBonded =>
    'The identity is not in the bond store and no pairing window was open: this was a '
        "stranger's connect request.",
  bleVerdictRefusedOutsideWindow =>
    'The phone paired on a link that may not make a bond, so that bond was forgotten.',
  bleVerdictRefusedUnsecured =>
    'The link failed the security policy: no bond, or a passkey that was not used.',
  bleVerdictRefusedBusy =>
    'A session was already live, or the block had the radio switched off.',
  _ => 'The device reported a verdict this build does not know.',
};

/// Whether the device's block enumeration carries the BLE Settings block. It is compiled into the
/// core only, so a node's absence of it is the normal case rather than an error.
bool hasBleSettingsBlock(Iterable<({int type, int maxInstance})> types) =>
    types.any((t) => t.type == bleSettingsBlockTypeValue);

/// The address part of a `Last peer` / `Last air` reading: the `AA:BB:CC:DD:EE:FF` before the
/// `/N` address type, or the whole text when the device sent no suffix.
///
/// The type is **information only** — NimBLE normalises the controller's resolved types 2/3 to
/// 0/1 — so two readings of the same address must compare equal however their types differ. This
/// is the one place that split happens for the app.
String bleAddressPart(String text) {
  final slash = text.indexOf('/');
  return slash < 0 ? text : text.substring(0, slash);
}

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
    this.bondCount = 0,
    this.bonds = const <String>[],
    this.windowMs = 0,
    this.enabled = false,
    this.displayName = '',
    this.advertisedName = '',
    this.lastPeer = '',
    this.lastAir = '',
    this.lastLink = bleVerdictNone,
    this.answered = const <int>{},
  });

  /// Builds the snapshot from the field reads: [fields] keyed by field number (key 0), and
  /// [bonds] keyed by the Bond field's key (0..2).
  ///
  /// A field the device did not answer keeps its empty default here (an unanswered passkey stays
  /// `''`, not `000000`) and is NOT recorded in [answered]. The defaults alone cannot be told
  /// apart from a device really reporting "no passkey, closed, off", so the panel asks
  /// [answeredField] before printing a field and shows `-` for one it never heard about.
  factory BleSecuritySnapshot.fromReads(
    Map<int, BleFieldRead> fields, {
    Map<int, BleFieldRead> bonds = const <int, BleFieldRead>{},
  }) {
    List<int> bytesOf(int field) => fields[field]?.value ?? const <int>[];
    final count = bytesOf(bleFieldBondCount);
    final window = bytesOf(bleFieldWindow);
    return BleSecuritySnapshot(
      passkey: decodePaddedString(bytesOf(bleFieldPasskey)),
      mode: bytesOf(bleFieldMode).isEmpty
          ? bleModeJustWorks
          : bytesOf(bleFieldMode)[0],
      bondCount: count.isEmpty ? 0 : count[0],
      bonds: [
        for (var key = 0; key < bleBondSlots; key++)
          if (decodePaddedString(bonds[key]?.value ?? const <int>[]).isNotEmpty)
            decodePaddedString(bonds[key]!.value),
      ],
      windowMs: window.length >= 4 ? uint32FromBytes(window) : 0,
      enabled: bytesOf(bleFieldEnabled).any((b) => b != 0),
      displayName: decodePaddedString(bytesOf(bleFieldDisplayName)),
      advertisedName: decodePaddedString(bytesOf(bleFieldAdvertisedName)),
      lastPeer: decodePaddedString(bytesOf(bleFieldLastPeer)),
      lastAir: decodePaddedString(bytesOf(bleFieldLastAir)),
      lastLink: bytesOf(bleFieldLastLink).isEmpty
          ? bleVerdictNone
          : bytesOf(bleFieldLastLink)[0],
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

  /// Entries in the device's bond store.
  final int bondCount;

  /// The bonded identities the block exposes, oldest first. Addresses only: the NimBLE store
  /// keeps no peer name (Issues.md E60), so the app must never promise one.
  final List<String> bonds;

  /// Milliseconds left in the connectable window; 0 means closed.
  final int windowMs;

  /// Whether the block wants the radio used at all.
  final bool enabled;

  /// The name the user set for the advertisement; empty means the System `Name` is used.
  final String displayName;

  /// The resolved name actually on the air (set by the device, not computed here).
  final String advertisedName;

  /// The identity the last admission decision used (`Last peer`), or empty while no link has been
  /// judged since boot. An address only, `AA:BB:CC:DD:EE:FF/N`: the block carries no peer name.
  final String lastPeer;

  /// The address the last judged peer connected from (`Last air`), `AA:BB:CC:DD:EE:FF/N`, or empty
  /// while no link has been judged since boot. It differs from [lastPeer] exactly when the
  /// controller resolved a rotating private address to the identity the gate compared with the bond
  /// store — the read-out E66 exists for ([lastAddressResolved]). Equal addresses are the expected
  /// reading for a peer that connects from its own public or static address, not a failed read.
  final String lastAir;

  /// How the last judged link ended, as one of the `bleVerdict*` values.
  final int lastLink;

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
          answered.contains(bleFieldBondCount));

  /// True when a passkey protects pairing (the device's own `Mode` says so).
  bool get passkeySet => mode == bleModePasskey;

  /// True while the device is discoverable and connectable.
  bool get windowOpen => windowMs > 0;

  /// True when the last judged link came from a different address than the identity the device
  /// judged: the controller resolved a rotating private address to the bonded identity.
  ///
  /// "Resolved" means exactly this: [lastPeer] carries the peer's identity and [lastAir] a
  /// DIFFERENT address on the air, for the same link. Only the address parts are compared
  /// ([bleAddressPart]) — the `/N` type suffix is information NimBLE normalises, so it can never
  /// fabricate a difference. Equal addresses mean the peer connected from its own public or static
  /// address, which is the normal reading (this rig's host) and not a failed read-out. Both
  /// addresses must have been read, so an unanswered field is never read as "resolved".
  bool get lastAddressResolved =>
      lastPeer.isNotEmpty &&
      lastAir.isNotEmpty &&
      bleAddressPart(lastPeer) != bleAddressPart(lastAir);

  /// True when the radio is on but nothing else is known: no passkey, no bonds, no window and no
  /// name. That is a block a build reports without driving a radio, and the panel says so instead
  /// of drawing a working link.
  bool get isEmpty =>
      !enabled &&
      !passkeySet &&
      bondCount == 0 &&
      !windowOpen &&
      displayName.isEmpty &&
      advertisedName.isEmpty;

  /// The state the panel reports for this reading.
  BleSecurityCondition get condition => bleSecurityCondition(this);

  @override
  String toString() =>
      'BleSecuritySnapshot(mode: ${bleModeLabel(mode)}, bonds: $bondCount, '
      'window: ${bleWindowText(windowMs)}, enabled: $enabled, '
      'advertised: "$advertisedName")';
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

/// The line under the last-link read-out: what the three E66 fields add up to, or null when the
/// device did not answer enough of them for the app to say anything (the panel then prints `-`
/// and makes no claim).
String? bleLastLinkDetail(BleSecuritySnapshot snapshot) {
  if (!snapshot.answeredField(bleFieldLastPeer) ||
      !snapshot.answeredField(bleFieldLastAir) ||
      !snapshot.answeredField(bleFieldLastLink)) {
    return null;
  }
  if (snapshot.lastPeer.isEmpty || snapshot.lastAir.isEmpty) {
    return snapshot.lastPeer.isEmpty && snapshot.lastAir.isEmpty
        ? 'No connect has been judged since the device booted: a power cycle clears these '
              'fields.'
        : null;
  }
  if (snapshot.lastAddressResolved) {
    return 'The two addresses differ: the phone connects from a private address the controller '
        'rotates, and the device judged it as the identity above. That is a bonded phone '
        'working normally.';
  }
  return 'The phone connected from the address the device knows it by: no address resolution '
      'was needed.';
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
  /// the device holds no bond for is not silent in the same way — since E66 the device answers its
  /// connect request and refuses the link once it cannot prove a bond, so the refusal normally
  /// reaches the phone as a failed pairing ([securityRefused]). Some stacks report a
  /// refuse-then-drop as a bare timeout anyway, which is what the device's USB panel settles
  /// (`Last link`, [bleVerdictDetail]).
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
            'the normal result of a firmware update, of "Forget all bonds", or of changing the '
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
  final text = (errorText ?? '').toLowerCase();
  if (text.isNotEmpty &&
      _securityFailureTokens.any((token) => text.contains(token))) {
    return BleLinkSymptom.securityRefused;
  }
  if (droppedWhileSecuring) {
    // A link that came up and died before it was ever usable is the shape of the device's
    // secure-link deadline, not of a device out of reach.
    return BleLinkSymptom.droppedDuringSecuring;
  }
  if (text.isNotEmpty &&
      _unreachableTokens.any((token) => text.contains(token))) {
    return BleLinkSymptom.unreachable;
  }
  return BleLinkSymptom.unknown;
}
