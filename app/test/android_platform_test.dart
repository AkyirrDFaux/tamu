import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/host_files.dart';
import 'package:tamuapp/core/platform_caps.dart';
import 'package:tamuapp/ui/widgets.dart';

/// Android/platform-gating tests (Docs/App/General info.md: Android = BLE,
/// Linux = BLE + USB). These run on the host; the target platform is switched
/// through [debugDefaultTargetPlatformOverride].
void main() {
  test('link source defaults to BLE on a BLE-only platform', () {
    // Must run before anything else touches the ConnectionManager singleton,
    // which picks its source at construction time.
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    expect(ConnectionManager.instance.source, LinkSource.ble);
    debugDefaultTargetPlatformOverride = null;
  });

  test('platform capabilities follow the documented target matrix', () {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    expect(isAndroid, isTrue);
    expect(isMobile, isTrue);
    expect(supportsUsb, isFalse);

    debugDefaultTargetPlatformOverride = TargetPlatform.linux;
    expect(isAndroid, isFalse);
    expect(isMobile, isFalse);
    expect(supportsUsb, isTrue);

    debugDefaultTargetPlatformOverride = null;
  });

  test('USB links are refused on a BLE-only platform', () async {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    addTearDown(() => debugDefaultTargetPlatformOverride = null);
    final error = await ConnectionManager.instance.connectTo(const DiscoveredLink(
        id: '/dev/ttyACM1', type: LinkType.usb, name: 'USB'));
    expect(error, isNotNull);
    expect(error, contains('USB is not supported'));
  });

  test('host file helpers round-trip bytes', () async {
    final dir = await Directory.systemTemp.createTemp('tamu_host_files');
    addTearDown(() => dir.delete(recursive: true));
    final path = '${dir.path}/payload.bin';
    await writePlatformFile(path, [0, 1, 2, 255]);
    expect(readPlatformFile(path), [0, 1, 2, 255]);
  });

  testWidgets('DialogBody shrinks to the available dialog width',
      (tester) async {
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: Center(
          child: SizedBox(
            width: 280,
            child: DialogBody(maxWidth: 520, child: const Text('body')),
          ),
        ),
      ),
    ));
    expect(
        tester.getSize(find.byType(DialogBody)).width, lessThanOrEqualTo(280));
  });

  testWidgets('shell uses the compact drawer layout on a phone width',
      (tester) async {
    tester.view.physicalSize = const Size(400, 800);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    late bool compact;
    await tester.pumpWidget(MaterialApp(
      home: Builder(builder: (context) {
        compact = ShellLayout.isCompact(context);
        return const SizedBox.shrink();
      }),
    ));
    expect(compact, isTrue);
  });
}
