/// Platform capability predicates.
///
/// Target matrix (Docs/App/General info.md): Android = BLE, Linux = BLE + USB,
/// Windows = not yet. These read Flutter's [defaultTargetPlatform] instead of
/// `dart:io`'s `Platform`, so widget/unit tests can switch platforms through
/// `debugDefaultTargetPlatformOverride`.
library;

import 'package:flutter/foundation.dart';

/// True on Android (the only mobile target the docs list).
bool get isAndroid =>
    !kIsWeb && defaultTargetPlatform == TargetPlatform.android;

/// True on phone/tablet targets.
bool get isMobile =>
    !kIsWeb &&
    (defaultTargetPlatform == TargetPlatform.android ||
        defaultTargetPlatform == TargetPlatform.iOS);

/// USB serial links are a desktop feature; Android is BLE-only per the docs.
bool get supportsUsb =>
    !kIsWeb &&
    (defaultTargetPlatform == TargetPlatform.linux ||
        defaultTargetPlatform == TargetPlatform.macOS ||
        defaultTargetPlatform == TargetPlatform.windows);
