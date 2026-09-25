/// Guard: the app manages subscriptions *on the devices* but must never register itself as a
/// requester or provider. `appSourceId` (0xFFFE) is the request source the app uses to talk to
/// a device; using it as a subscription requester address would make the app the subscriber.
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

void main() {
  test('the app never registers itself as a subscription requester/provider', () {
    final files = [
      'lib/core/subscription_client.dart',
      'lib/core/backup.dart',
      'lib/ui/subscriptions_page.dart',
      'lib/ui/subscriptions_dialog.dart',
    ];
    for (final path in files) {
      final source = File(path).readAsStringSync();
      expect(source.contains('appSourceId'), isFalse,
          reason: '$path must not use the app address as a subscription address');
    }
    // The requester address written on the device's behalf is the device's own id.
    final client = File('lib/core/subscription_client.dart').readAsStringSync();
    expect(client.contains('requester address = us'), isTrue,
        reason: 'requester entries carry the device address, not the app\'s');
  });
}
