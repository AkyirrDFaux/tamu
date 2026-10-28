import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/wifi_settings.dart';
import 'package:tamuapp/ui/formatters.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/theme.dart';

/// The WiFi Settings block on the generic Register view: the registry's names replace
/// "Field N", `State` prints its own label rather than an enum index, and the password row
/// masks the value on the one surface that renders every block's values.
class _WifiRegisterClient extends RegisterClient {
  _WifiRegisterClient({required super.deviceId});

  static const int _persistent = ValueFlags.persistent;
  static const int _readOnly = ValueFlags.readOnly;

  @override
  Future<BlocksRead?> readBlocks({List<int>? scriptSlots}) async => (
    blocks: <ReportedBlock>[
      (
        type: wifiSettingsBlockTypeValue,
        inst: 0,
        meta: ValueInfo(type: wifiSettingsBlockTypeValue, size: wifiFieldCount),
        name: 'WiFi',
      ),
    ],
    unreadTypes: const <int>{},
  );

  @override
  Future<StaticFieldLayout> readStaticFieldLayout() async => const {};

  @override
  Future<({ValueInfo meta, List<int> value})?> readBlockField(
    int blockType,
    int instance,
    int field,
    int key,
  ) async {
    ValueInfo info(int type, int size, int flags) =>
        ValueInfo(type: type, size: size, flags: flags);
    List<int> text(String s, int width) {
      final bytes = s.codeUnits.toList();
      bytes.addAll(List<int>.filled(width - bytes.length, 0));
      return bytes;
    }

    return switch (field) {
      wifiFieldEnabled => (
        meta: info(DataType.bool_.value, 1, _persistent),
        value: const [1],
      ),
      wifiFieldSsid => (
        meta: info(DataType.text.value, 32, _persistent),
        value: text('TamuNet', 32),
      ),
      wifiFieldPassword => (
        meta: info(DataType.text.value, 32, _persistent),
        value: text('hunter2', 32),
      ),
      wifiFieldState => (
        meta: info(DataType.enum_.value, 1, _readOnly),
        value: const [2], // Connected
      ),
      wifiFieldIp => (
        // The address is 16 raw Undefined bytes in network order, IPv4-mapped.
        meta: info(DataType.undefined.value, 16, _readOnly),
        value: [for (var i = 0; i < 10; i++) 0, 0xFF, 0xFF, 192, 168, 0, 9],
      ),
      _ => null,
    };
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('the WiFi block renders named, with a masked password', (
    tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: RegisterPage(
          deviceId: 1,
          isConnected: () => true,
          clientFactory: (_) => _WifiRegisterClient(deviceId: 1),
        ),
      ),
    );
    await tester.pump(); // the initial topology load
    await tester.tap(find.text('WiFi').first); // expand the block
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(tester.takeException(), isNull);

    Finder rowOf(String label) => find
        .ancestor(of: find.text(label), matching: find.byType(ListTile))
        .first;

    // The registry's names, not "Field 0../Field 4".
    expect(find.text('Enabled'), findsOneWidget);
    expect(find.text('SSID'), findsOneWidget);
    expect(find.text('Password'), findsOneWidget);
    expect(find.text('State'), findsOneWidget);
    expect(find.text('IP address'), findsOneWidget);
    expect(find.textContaining('Field 0'), findsNothing);
    expect(find.textContaining('Field 4'), findsNothing);

    // State prints its label, IP prints the address it holds.
    expect(
      find.descendant(of: rowOf('State'), matching: find.text('Connected')),
      findsOneWidget,
    );
    // The address is named (not "Field 4") and its raw bytes print as hex on this surface; the
    // WiFi panel is the one that formats it as an address.
    expect(
      find.descendant(
        of: rowOf('IP address'),
        matching: find.text('00 00 00 00 00 00 00 00 00 00 ff ff c0 a8 00 09'),
      ),
      findsOneWidget,
    );

    // The password is masked on this surface too, and the value is nowhere in the tree.
    expect(
      find.descendant(of: rowOf('Password'), matching: find.text(kSecretMask)),
      findsOneWidget,
    );
    expect(find.textContaining('hunter2'), findsNothing);

    await tester.pumpWidget(const SizedBox()); // dispose the auto-refresh timer
  });
}
