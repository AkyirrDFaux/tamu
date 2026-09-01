import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/bootloader_client.dart';
import '../core/connection.dart';
import '../core/protocol.dart';
import 'theme.dart';
import 'widgets.dart';

/// Bootloader page: scans the bus for devices in bootloader mode (broadcast
/// CID 0 check) and allows uploading firmware to any that respond.
class BootloaderPage extends StatefulWidget {
  const BootloaderPage({super.key});

  @override
  State<BootloaderPage> createState() => _BootloaderPageState();
}

class _BootloaderPageState extends State<BootloaderPage>
    with AutoRefreshMixin<BootloaderPage> {
  final List<_BootDevice> _devices = [];
  bool _scanning = false;
  bool _uploading = false;
  double _uploadProgress = 0;
  String _uploadStatus = '';
  int? _uploadTarget;

  @override
  int? get shellTabIndex => 4; // Bootloader tab

  @override
  void initState() {
    super.initState();
    _scan();
  }

  @override
  Future<void> onAutoRefresh() => _scan();

  @override
  void onAutoRefreshStarted() {
    _scan();
  }

  /// Broadcast CID 0 (Bootloader Check) on the bus and collect responses.
  Future<void> _scan() async {
    if (_scanning || !ConnectionManager.instance.isConnected) return;
    setState(() => _scanning = true);
    try {
      final mgr = ConnectionManager.instance;
      // Send a broadcast CID 0 check. The bootloader responds with
      // payload[0..3] = SN (copied from request, ignored) + payload[4] = bool.
      final reply = await mgr.request(
        0xFFFF, // broadcast
        ServiceType.bootloader,
        0,
        payload: Uint8List(4),
        timeout: const Duration(seconds: 2),
      );
      if (reply.length >= 5 && reply[4] != 0) {
        // A device is in bootloader mode. We can't distinguish multiple
        // devices from a single broadcast, so show one entry.
        if (!_devices.any((d) => d.broadcast)) {
          setState(() => _devices.add(_BootDevice.broadcast()));
        }
      }
    } catch (_) {
      // No device responded or timeout — that's fine.
    } finally {
      if (mounted) setState(() => _scanning = false);
    }
  }

  /// Prompt for a firmware file and upload it to the selected device.
  Future<void> _uploadFirmware(int targetAddr) async {
    final messenger = ScaffoldMessenger.of(context);
    try {
      final result = await FilePicker.pickFiles(
        type: FileType.custom,
        allowedExtensions: ['bin', 'hex'],
      );
      if (result == null || result.files.isEmpty) return;

      final file = result.files.first;
      Uint8List binary;
      if (file.extension == 'hex') {
        final content = await File(file.path!).readAsString();
        binary = _parseHex(content);
      } else {
        binary = Uint8List.fromList(await File(file.path!).readAsBytes());
      }

      if (binary.isEmpty) {
        if (mounted) {
          messenger.showSnackBar(const SnackBar(content: Text('File is empty')));
        }
        return;
      }

      final client = BootloaderClient(deviceId: targetAddr);

      setState(() {
        _uploading = true;
        _uploadTarget = targetAddr;
        _uploadProgress = 0;
        _uploadStatus = 'Checking bootloader...';
      });

      final inBootloader = await client.check();
      if (!inBootloader) {
        if (mounted) {
          setState(() {
            _uploading = false;
            _uploadTarget = null;
            _uploadStatus = '';
          });
          messenger.showSnackBar(const SnackBar(
              content: Text('Device not in bootloader mode')));
        }
        return;
      }

      setState(() {
        _uploadStatus = 'Erasing and uploading firmware...';
        _uploadProgress = 0;
      });

      final uploaded = await client.writeBinary(
        binary,
        onProgress: (current, total) {
          if (mounted) {
            setState(() => _uploadProgress = current / total);
          }
        },
      );

      if (mounted) {
        setState(() {
          _uploading = false;
          _uploadTarget = null;
          _uploadStatus = '';
          _uploadProgress = uploaded ? 1.0 : 0;
        });
        messenger.showSnackBar(SnackBar(
            content: Text(uploaded
                ? 'Firmware uploaded — reset device to boot new firmware'
                : 'Firmware upload failed')));
      }
    } catch (e) {
      if (mounted) {
        setState(() {
          _uploading = false;
          _uploadTarget = null;
          _uploadStatus = '';
        });
        messenger.showSnackBar(SnackBar(content: Text('Upload error: $e')));
      }
    }
  }

  /// Intel HEX to raw binary (same as DeviceViewPage).
  Uint8List _parseHex(String content) {
    final lines = content.split('\n');
    final data = <int>[];
    for (final line in lines) {
      final trimmed = line.trim();
      if (!trimmed.startsWith(':')) continue;
      if (trimmed.length < 11) continue;
      final byteCount = int.parse(trimmed.substring(1, 3), radix: 16);
      final recordType = int.parse(trimmed.substring(7, 9), radix: 16);
      if (recordType == 0x01) break;
      if (recordType != 0x00) continue;
      for (var i = 0; i < byteCount; i++) {
        data.add(int.parse(trimmed.substring(9 + i * 2, 11 + i * 2), radix: 16));
      }
    }
    return Uint8List.fromList(data);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Bootloader'),
        actions: [
          if (_uploading)
            const Padding(
              padding: EdgeInsets.all(16),
              child: SizedBox(
                  width: 18,
                  height: 18,
                  child: CircularProgressIndicator(strokeWidth: 2)),
            )
          else
            RefreshButton(
              onRefresh: _scan,
              autoActive: autoRefreshActive,
              refreshing: _scanning,
              error: false,
              selectedInterval: selectedInterval,
              onSelectAuto: applyAuto,
            ),
        ],
      ),
      body: _devices.isEmpty
          ? Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(Icons.usb_off,
                      size: 64, color: Colors.white.withValues(alpha: 0.3)),
                  const SizedBox(height: 16),
                  Text('No bootloader devices found',
                      style: Theme.of(context).textTheme.bodyLarge),
                  const SizedBox(height: 8),
                  Text(
                    'Hold the boot button and reset a device,\n'
                    'then press refresh.',
                    textAlign: TextAlign.center,
                    style: Theme.of(context).textTheme.bodySmall,
                  ),
                ],
              ),
            )
          : ListView.builder(
              padding: const EdgeInsets.all(12),
              itemCount: _devices.length,
              itemBuilder: (context, index) =>
                  _deviceCard(context, _devices[index]),
            ),
    );
  }

  Widget _deviceCard(BuildContext context, _BootDevice dev) {
    final isUploading = _uploading && _uploadTarget == dev.addr;
    return Card(
      color: kSurfaceAlt,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(children: [
              const Icon(Icons.memory, color: kOrange, size: 20),
              const SizedBox(width: 8),
              Text('Device @ 0x${dev.addr.toRadixString(16).toUpperCase().padLeft(4, '0')}',
                  style: const TextStyle(fontWeight: FontWeight.w600)),
              const Spacer(),
              if (dev.broadcast)
                const Chip(
                  label: Text('Broadcast', style: TextStyle(fontSize: 11)),
                  visualDensity: VisualDensity.compact,
                  materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                ),
            ]),
            const SizedBox(height: 8),
            if (isUploading) ...[
              Text(_uploadStatus,
                  style: Theme.of(context).textTheme.bodySmall),
              const SizedBox(height: 8),
              LinearProgressIndicator(value: _uploadProgress),
              const SizedBox(height: 4),
              Text('${(_uploadProgress * 100).toStringAsFixed(0)}%',
                  style: Theme.of(context).textTheme.bodySmall),
            ] else ...[
              Text(
                'Ready to upload firmware. The device will flash\n'
                'the new binary and reset when done.',
                style: Theme.of(context).textTheme.bodySmall,
              ),
              const SizedBox(height: 12),
              Align(
                alignment: Alignment.centerRight,
                child: FilledButton.icon(
                  icon: const Icon(Icons.system_update_alt, size: 18),
                  label: const Text('Upload Firmware'),
                  onPressed: () => _uploadFirmware(dev.addr),
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

/// Represents a device discovered in bootloader mode.
class _BootDevice {
  final int addr;
  final bool broadcast;

  _BootDevice({required this.addr, required this.broadcast});

  /// Factory for a broadcast-discovered device (address unknown, will use
  /// broadcast for uploads).
  factory _BootDevice.broadcast() =>
      _BootDevice(addr: 0xFFFF, broadcast: true);
}
