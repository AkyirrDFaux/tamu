/// The update page has a way out of a flash.
///
/// A flash writes every 32-byte chunk of an image (~13,500 for the app space) and reads them all
/// back; when the node leaves bootloader mode after `identify()` succeeded the page used to crawl
/// with no cancel at all (the card's report: `update_page.dart:165`). The client now stops at the
/// first chunk that does not land, and this file pins the page's half: the Cancel control appears
/// while a run is in flight, and tapping it ends the run after the chunk in flight.
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/ui/update_page.dart';

/// A bootloader channel whose first write is held open until the test lets it finish: the chunk
/// in flight when the user cancels.
class _HeldWriteTransport implements BootloaderTransport {
  _HeldWriteTransport(this.hold);

  final Completer<void> hold;
  final flash = <int, Uint8List>{};
  int writes = 0;

  @override
  Future<WriteAck> writeChunk(
    int offset,
    List<int> data,
    Duration timeout,
  ) async {
    writes++;
    if (writes == 1) await hold.future;
    flash[offset] = Uint8List.fromList(data);
    return WriteAck(true, lastWrittenOffset: offset);
  }

  @override
  Future<Uint8List?> readChunk(int offset, Duration timeout) async =>
      flash[offset];

  @override
  Future<int?> digest(int offset, int length, Duration timeout) async {
    // Hold the whole padded range: what the page's run would verify if it got that far.
    var crc = 0xFFFFFFFF;
    for (var i = offset; i < offset + length; i++) {
      final held = flash[i - i % 32];
      final b = held == null ? 0xFF : held[i % 32];
      crc ^= b;
      for (var bit = 0; bit < 8; bit++) {
        crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320 : 0);
      }
    }
    return crc ^ 0xFFFFFFFF;
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async =>
      const BootloaderIdentity(appSize: 0x4000, name: 'Tamu v2.0A');
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
    // Four chunks: 128 bytes at the bootloader's 32-byte payload.
    final image = Uint8List.fromList(List.generate(128, (i) => i & 0xFF));

    await tester.pumpWidget(
      MaterialApp(
        home: UpdatePage(
          isConnected: () => true,
          initialImage: image,
          clientFactory: () => BootloaderClient(
            transport: transport,
            writePacing: Duration.zero,
          ),
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

    // The chunk in flight lands, and the next check ends the run.
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
