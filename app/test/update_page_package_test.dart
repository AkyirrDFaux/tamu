/// The Update page writes only from a package it has checked.
///
/// The page's own half of the package gate: a bare `.bin` and a damaged package never become the
/// chosen image, a refusal is shown by name instead of a skipped flash, and switching the route
/// re-checks the same file against the other target so the wrong package cannot stay selected.
library;

import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/ui/update_page.dart';

import 'support/fw_package.dart';

/// Mounts the page with no client ever built: every case here ends before a flash.
Future<void> mount(WidgetTester tester, UpdatePage page) async {
  tester.view.physicalSize = const Size(1000, 3000);
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(MaterialApp(home: page));
  await tester.pumpAndSettle();
}

/// Whether the Update button can be pressed.
bool canUpdate(WidgetTester tester) =>
    tester
        .widget<FilledButton>(find.widgetWithText(FilledButton, 'Update'))
        .onPressed !=
    null;

void main() {
  testWidgets('update page lays out and gates the Update button', (tester) async {
    // The page is a tall ListView; give it a surface big enough to build every card.
    tester.view.physicalSize = const Size(1000, 2400);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(const MaterialApp(home: UpdatePage()));
    await tester.pumpAndSettle();

    // The guide, target, package and log sections all render.
    expect(find.text('How to update'), findsOneWidget);
    expect(find.text('Target'), findsOneWidget);
    expect(find.text('Package'), findsOneWidget);
    expect(find.text('Log'), findsOneWidget);
    expect(find.text('Choose package'), findsOneWidget);

    // Without a chosen package (and with no link) the Update button is disabled.
    final button = tester.widget<FilledButton>(
      find.widgetWithText(FilledButton, 'Update'),
    );
    expect(button.onPressed, isNull);
  });

  testWidgets('a package for the route is reported as checked', (tester) async {
    final fake = PackageForTest(target: 'DAS_v0_1');
    await mount(
      tester,
      UpdatePage(
        isConnected: () => true,
        initialPackage: fake.build(),
        initialPackageName: fake.name,
      ),
    );

    expect(find.text('Package'), findsOneWidget);
    expect(find.text(fake.name), findsOneWidget);
    expect(find.text('DAS_v0_1 at 0x08000800'), findsOneWidget);
    expect(find.text('26.11.12.4 · abc123def456'), findsOneWidget);
    expect(
      find.textContaining(hex32(Bootloader.crc32Padded(fake.image))),
      findsWidgets,
      reason:
          'the card (and the log) show the digest the device is '
          'compared with',
    );
    expect(canUpdate(tester), isTrue);
  });

  testWidgets('a bare .bin is refused, naming the package it wants', (
    tester,
  ) async {
    await mount(
      tester,
      UpdatePage(
        isConnected: () => true,
        pickFile: () async => PickedFirmwareFile(
          'DAS_v0_1.bin',
          Uint8List.fromList(List.generate(128, (i) => i & 0xFF)),
        ),
      ),
    );

    await tester.tap(find.widgetWithText(OutlinedButton, 'Choose package'));
    await tester.pumpAndSettle();

    expect(find.textContaining('DAS_v0_1.bin is a bare image'), findsOneWidget);
    expect(
      find.textContaining('binaries/DAS_v0_1-<version>.fw.zip'),
      findsOneWidget,
      reason: 'the refusal says which package carries the image',
    );
    expect(find.textContaining('nothing was written'), findsOneWidget);
    expect(
      canUpdate(tester),
      isFalse,
      reason: 'a refused file has no bytes the page may write',
    );
  });

  testWidgets('a damaged package is refused on the page, with its offset', (
    tester,
  ) async {
    final fake = PackageForTest(target: 'DAS_v0_1');
    final damaged = Uint8List.fromList(fake.image);
    damaged[0x40] ^= 0x5A;
    fake.payload = damaged;

    await mount(
      tester,
      UpdatePage(
        isConnected: () => true,
        initialPackage: fake.build(),
        initialPackageName: fake.name,
      ),
    );

    expect(find.text('refused - see below'), findsOneWidget);
    expect(find.textContaining('offset 0x00000040'), findsOneWidget);
    expect(find.textContaining('the manifest expects 0x40'), findsOneWidget);
    expect(canUpdate(tester), isFalse);
  });

  testWidgets('switching the route re-checks the chosen package', (
    tester,
  ) async {
    final fake = PackageForTest(target: 'DAS_v0_1');
    await mount(
      tester,
      UpdatePage(
        isConnected: () => true,
        initialPackage: fake.build(),
        initialPackageName: fake.name,
      ),
    );
    expect(canUpdate(tester), isTrue);

    await tester.tap(find.byType(SwitchListTile));
    await tester.pumpAndSettle();

    expect(
      find.textContaining('the package is for DAS_v0_1, but this route'),
      findsOneWidget,
      reason: 'the core route writes Tamu_v2_0A, so this file is not its image',
    );
    expect(canUpdate(tester), isFalse);
  });

  testWidgets('a package for the node route verifies once it is chosen', (
    tester,
  ) async {
    final fake = PackageForTest(target: 'DAS_v0_1');
    await mount(
      tester,
      UpdatePage(
        isConnected: () => true,
        pickFile: () async => PickedFirmwareFile(fake.name, fake.build()),
      ),
    );

    await tester.tap(find.widgetWithText(OutlinedButton, 'Choose package'));
    await tester.pumpAndSettle();
    await tester.tap(find.byType(SwitchListTile));
    await tester.pumpAndSettle();

    expect(find.text('DAS v0.1 at 0x08000800'), findsOneWidget);
    expect(canUpdate(tester), isTrue);
  });

  testWidgets('flashPackage is called and logs version from manifest', (
    tester,
  ) async {
    final fake = PackageForTest(target: 'DAS_v0_1');
    var identifyCalled = false;

    await mount(
      tester,
      UpdatePage(
        isConnected: () => true,
        initialPackage: fake.build(),
        initialPackageName: fake.name,
        clientFactory: () => BootloaderClient(
          transport: _TestFlashPackageTransport(
            onIdentify: () {
              identifyCalled = true;
              return Future.value(
                BootloaderIdentity(appSize: 2048, pageSize: 64, name: 'Test'),
              );
            },
          ),
        ),
      ),
    );

    expect(canUpdate(tester), isTrue);

    await tester.tap(find.widgetWithText(FilledButton, 'Update'));
    await tester.pumpAndSettle();
    // Wait for the async flash operations to complete
    await tester.pump(const Duration(seconds: 1));

    expect(identifyCalled, isTrue, reason: 'identify should be called');

    // The log should show the version from the manifest (added by flashPackage).
    expect(find.textContaining('Version: ${fake.version}'), findsOneWidget);
    expect(find.textContaining('abc123def456'), findsOneWidget);
    expect(find.textContaining('target DAS_v0_1'), findsOneWidget);
  });
}

/// A test transport that simulates a successful flash without throwing.
class _TestFlashPackageTransport implements BootloaderTransport {
  _TestFlashPackageTransport({required this.onIdentify});

  final Future<BootloaderIdentity> Function() onIdentify;

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async =>
      onIdentify();

  @override
  Future<bool> enterBootloader(Duration timeout) async => true;

  @override
  Future<Uint8List> readChunk(int offset, Duration timeout) async =>
      Uint8List(32);

  @override
  Future<int> erasePage(int offset, Duration timeout) async => offset;

  @override
  Future<WriteAck> writeChunk(
    int offset,
    List<int> data,
    Duration timeout,
  ) async =>
      WriteAck(true, acked: offset);

  @override
  Future<int> crc(Duration timeout) async => 0;

  @override
  Future<bool> jump(Duration timeout) async => true;
}
