# Tamu App

Flutter companion app for the Tamu device network.

Complementary firmware: https://github.com/AkyirrDFaux/tamu

## Status (per Docs/App/General info.md)

| OS      | Connection types | Status               |
| ------- | ---------------- | -------------------- |
| Android | BLE              | In development       |
| Linux   | BLE, USB         | In development       |
| Windows | BLE, USB         | Do not implement yet |

## Structure

```
lib/
  main.dart            App entry, shell (drawer on phones / rail on desktop)
  core/
    protocol.dart      Generic packet framing, CRC8, transaction stream parser
    types.dart         Data formats: Number 16.16, BlockIndex/BlockMeta, enums
    transport.dart     Link transports + USB/BLE framing per Services/App Interface
    platform_caps.dart Platform gating (Android = BLE, desktop = BLE + USB)
    ble_permissions.dart Android BLE runtime permissions
    connection.dart    Scanning, link management, CID transaction layer
    device_db.dart     In-RAM network database (Device service / SNDB via ID 1)
    backup.dart        Whole-network zip backup / live restore
    settings.dart      Settings store
    host_files.dart    Host file pick/save helpers
  ui/                  One file per documented page
```

## Build

```
flutter pub get
flutter analyze
flutter test
flutter build linux
flutter build apk --debug
```

### Android prerequisites

- Android SDK: set once with `flutter config --android-sdk <path>`.
- **JDK 17-24.** Gradle 8.14 and the Kotlin build tooling reject newer JDKs
  (Java 25/26 fail with `java.lang.IllegalArgumentException: 25.0.3` from
  Kotlin's `JavaVersion.parse`). Point Flutter at a supported JDK with
  `flutter config --jdk-dir=<path>`.
- Android is BLE only (no USB), with runtime permissions requested on first
  scan; settings live in the app support directory.
- `permission_handler` is pinned to `^11.3.1`: 13.x's Android implementation
  needs `compileSdk 37`, which this SDK ships as `android-37.0` (unresolvable by
  AGP 8.11). See `Issues.md`.
- Beta builds are labelled **Tamu App (beta)** and use the application id
  **`tamu.app.beta`** (`applicationIdSuffix = ".beta"` in
  `android/app/build.gradle.kts`), so they install **alongside** the previous
  `tamu.app` release instead of updating it. Debug and release share that id, so
  installing one replaces the other. Remove the suffix to promote the beta.
