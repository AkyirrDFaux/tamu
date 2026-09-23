/// Android BLE runtime permissions.
///
/// universal_ble requires the app to request permissions before scanning
/// (Android 12+ `BLUETOOTH_SCAN`/`BLUETOOTH_CONNECT`; Android 11 and below the
/// legacy location permission). permission_handler maps both Bluetooth
/// permissions onto the legacy location permission on Android 11 and below
/// (`PermissionUtils.determineBluetoothPermission`), so a single request pair
/// covers every supported Android version.
library;

import 'package:permission_handler/permission_handler.dart';

import 'diagnostics.dart';
import 'platform_caps.dart';

/// Outcome of the BLE runtime-permission request.
enum BlePermissionResult { granted, denied, permanentlyDenied }

/// Requests the runtime permissions needed to scan and connect. Always
/// [BlePermissionResult.granted] off Android.
Future<BlePermissionResult> ensureBlePermissions() async {
  if (!isAndroid) return BlePermissionResult.granted;
  final statuses = await <Permission>[
    Permission.bluetoothScan,
    Permission.bluetoothConnect,
  ].request();
  final scan = statuses[Permission.bluetoothScan];
  final connect = statuses[Permission.bluetoothConnect];
  if ((scan?.isGranted ?? false) && (connect?.isGranted ?? false)) {
    AppDiagnostics.log('ble', 'runtime permissions granted');
    return BlePermissionResult.granted;
  }
  AppDiagnostics.log(
      'ble', 'runtime permissions not granted (scan=$scan, connect=$connect)');
  final permanent = (scan?.isPermanentlyDenied ?? false) ||
      (connect?.isPermanentlyDenied ?? false);
  return permanent
      ? BlePermissionResult.permanentlyDenied
      : BlePermissionResult.denied;
}

/// Opens the app's system settings page (used when a permission is permanently
/// denied and can only be re-enabled there).
Future<void> openBlePermissionSettings() => openAppSettings();
