import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/wifi_settings.dart';
import 'package:tamuapp/ui/formatters.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/theme.dart';

import 'support/block_reads.dart';
import 'support/single_block_client.dart';

/// The WiFi Settings block on the generic Register view: the registry's names replace
/// "Field N", `State` prints its own label rather than an enum index, and the password row
/// masks the value on the one surface that renders every block's values.
class _WifiRegisterClient extends SingleBlockRegisterClient {
  _WifiRegisterClient({required super.deviceId});

  @override
  int get blockType => wifiSettingsBlockTypeValue;

  @override
  String get blockName => 'WiFi';

  @override
  int get fieldCount => wifiFieldCount;

  @override
  Future<BlockRead?> readBlockField(
    int blockType,
    int instance,
    int field,
    int key,
  ) async {
    const persistent = ValueFlags.persistent;
    const readOnly = ValueFlags.readOnly;

    return switch (field) {
      wifiFieldEnabled => fieldBytes(DataType.bool_.value, const [
        1,
      ], flags: persistent),
      wifiFieldSsid => fieldBytes(
        DataType.text.value,
        paddedText('TamuNet', 32),
        flags: persistent,
      ),
      wifiFieldPassword => fieldBytes(
        DataType.text.value,
        paddedText('hunter2', 32),
        flags: persistent,
      ),
      wifiFieldState => fieldBytes(DataType.enum_.value, const [
        2,
      ], flags: readOnly), // Connected
      wifiFieldIp => fieldBytes(
        // The address is 16 raw Undefined bytes in network order, IPv4-mapped.
        DataType.undefined.value,
        [for (var i = 0; i < 10; i++) 0, 0xFF, 0xFF, 192, 168, 0, 9],
        flags: readOnly,
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
