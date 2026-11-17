/// The update page has a way out of a flash.
///
/// A flash reads every page of the image, writes the ones that differ, blanks the tail and then
/// verifies twice; against a node that left bootloader mode after `identify()` succeeded the page
/// used to crawl with no cancel at all. The client checks `isCancelled` between pages, and this
/// file pins the page's half: the Cancel control appears while a run is in flight, and tapping it
/// ends the run at the next page boundary, after the chunk in flight.
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/ui/update_page.dart';

import 'support/fw_package.dart';

/// A bootloader channel whose first write is held open until the test lets it finish: the chunk
/// in flight when the user cancels.
///
/// The node reports a one-chunk page, so the cancel lands between the page the held chunk belongs
/// to and the next one: "the second chunk was never sent" then means exactly what it says.
class _HeldWriteTransport implements BootloaderTransport {
  _HeldWriteTransport(this.hold);

  final Completer<void> hold;
  final flash = <int, Uint8List>{};
  int writes = 0;

  static const int appSize = 0x4000;
  static const int pageSize = Bootloader.payloadSize;

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async =>
      const BootloaderIdentity(
        appSize: appSize,
        pageSize: pageSize,
        name: 'Tamu v2.0A',
      );

  @override
  Future<Uint8List> readChunk(int offset, Duration timeout) async =>
      flash[offset] ??
      (Uint8List(Bootloader.payloadSize)
        ..fillRange(0, Bootloader.payloadSize, 0xFF));

  @override
  Future<int> erasePage(int offset, Duration timeout) async {
    flash.removeWhere((o, _) => o >= offset && o < offset + pageSize);
    return offset;
  }

  @override
  Future<WriteAck> writeChunk(
    int offset,
    List<int> data,
    Duration timeout,
  ) async {
    writes++;
    if (writes == 1) await hold.future;
    flash[offset] = Uint8List.fromList(data);
    return WriteAck(true, acked: offset);
  }

  @override
  Future<int> crc(Duration timeout) async {
    final space = Uint8List(appSize)..fillRange(0, appSize, 0xFF);
    flash.forEach((offset, data) => space.setRange(offset, offset + data.length, data));
    return Bootloader.crc32(space);
  }

  @override
  Future<bool> jump(Duration timeout) async => false;

  @override
  Future<bool> enterBootloader(Duration timeout) async => false;
}

void main() {
  testWidgets('a running update can be cancelled from the page', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1000, 2400);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final hold = Completer<void>();
    final transport = _HeldWriteTransport(hold);
    // Four chunks: 128 bytes at the bootloader's 32-byte payload, in the node's package.
    final fakePackage = PackageForTest(
      target: 'DAS_v0_1',
      image: Uint8List.fromList(List.generate(128, (i) => i & 0xFF)),
    );

    await tester.pumpWidget(
      MaterialApp(
        home: UpdatePage(
          isConnected: () => true,
          initialPackage: fakePackage.build(),
          initialPackageName: fakePackage.name,
          clientFactory: () => BootloaderClient(transport: transport),
        ),
      ),
    );
    await tester.pumpAndSettle();

    await tester.tap(find.widgetWithText(FilledButton, 'Update'));
    await tester
        .pump(); // identify() answers, the first write starts and is held

    expect(
      transport.writes,
      1,
      reason: 'the run is in flight, parked on chunk 1',
    );
    expect(
      find.widgetWithText(OutlinedButton, 'Cancel'),
      findsOneWidget,
      reason: 'a run with no way out is the defect this card fixes',
    );

    await tester.tap(find.widgetWithText(OutlinedButton, 'Cancel'));
    await tester.pump();
    expect(find.textContaining('Stopping after the chunk'), findsOneWidget);

    // The chunk in flight lands, and the next page boundary ends the run.
    hold.complete();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 10));

    expect(
      find.textContaining('Update stopped'),
      findsWidgets,
      reason: 'the page reports the stop instead of leaving the bar crawling',
    );
    expect(find.textContaining('cancelled'), findsWidgets);
    expect(
      find.text('Cancel'),
      findsNothing,
      reason: 'nothing is running any more',
    );
    expect(transport.writes, 1, reason: 'the second chunk was never sent');
  });
}
