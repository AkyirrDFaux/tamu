# Tamu App

Flutter companion app for the Tamu device network.

Complementary firmware: https://github.com/AkyirrDFaux/tamu

## Status (per Docs/App/General info.md)

| OS      | Connection types | Status               |
| ------- | ---------------- | -------------------- |
| Android | BLE              | Do not implement yet |
| Linux   | BLE, USB         | In development       |
| Windows | BLE, USB         | Do not implement yet |

## Structure

```
lib/
  main.dart            App entry, shell layout (Connection/Devices/Backup/Settings)
  core/
    protocol.dart      Generic packet framing, CRC8, transaction stream parser
    types.dart         Data formats: Number 16.16, BlockIndex/BlockMeta, enums
    transport.dart     Link transports + USB/BLE framing per Services/App Interface
    ble_transport.dart BLE transport (universal_ble)
    usb_transport.dart USB serial transport (flutter_libserialport, 115200 8N1)
    connection.dart    Scanning, link management, CID transaction layer
    device_db.dart     In-RAM network database (Device service / SNDB via ID 1)
    sysmem.dart        System Memory service client
    backup.dart        Whole-network zip backup / live restore
    settings.dart      Settings store
  ui/                  One file per documented page
```

## Build

```
flutter pub get
flutter analyze
flutter test
flutter build linux
```
