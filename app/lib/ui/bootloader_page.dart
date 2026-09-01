import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/bootloader_client.dart';
import '../core/connection.dart';
import 'theme.dart';
import 'widgets.dart';

/// Bootloader page: manages the core's bootloader mode and streams firmware
/// to a connected node via the RS-Bus bridge.
///
/// Flow:
///   1. User clicks "Enter Bootloader Mode" -> CID 1 switch(true)
///   2. Core listens for node enumeration (user resets node with button held)
///   3. User picks firmware file and clicks "Upload"
///   4. App writes all fragments (CID 4), verifies (CID 3), then optionally leaves
class BootloaderPage extends StatefulWidget {
  const BootloaderPage({super.key});

  @override
  State<BootloaderPage> createState() => _BootloaderPageState();
}

class _BootloaderPageState extends State<BootloaderPage>
    with AutoRefreshMixin<BootloaderPage> {
  final BootloaderClient _client = BootloaderClient(deviceId: 1);

  bool _inBootloader = false;
  Uint8List? _vendorInfo;

  bool _uploading = false;
  bool _verifying = false;
  double _progress = 0;
  String _status = '';
  String? _error;

  @override
  int? get shellTabIndex => 4;

  @override
  void initState() {
    super.initState();
    _checkMode();
  }

  @override
  Future<void> onAutoRefresh() => _checkMode();

  @override
  void onAutoRefreshStarted() {}

  Future<void> _checkMode() async {
    if (!ConnectionManager.instance.isConnected) return;
    final inMode = await _client.check();
    if (!mounted) return;
    setState(() {
      _inBootloader = inMode;
      if (!inMode) _vendorInfo = null;
    });
    if (inMode) _fetchDeviceInfo();
  }

  Future<void> _fetchDeviceInfo() async {
    for (var i = 0; i < 10; i++) {
      final info = await _client.deviceInfo();
      if (info != null && info.length >= 16) {
        if (mounted) setState(() => _vendorInfo = info);
        return;
      }
      await Future.delayed(const Duration(seconds: 1));
    }
  }

  Future<void> _enterBootloader() async {
    setState(() {
      _status = 'Entering bootloader mode...';
      _error = null;
    });
    // CID 1 returns immediately — enumeration happens in the main loop.
    final ok = await _client.switchMode(enter: true);
    if (!mounted) return;
    if (!ok) {
      setState(() {
        _status = '';
        _error = 'Failed to enter bootloader mode';
      });
      return;
    }
    setState(() {
      _inBootloader = true;
      _status = 'Waiting for node enumeration...\nHold boot button and reset the node.';
    });
    // Poll CID 2 until the node enumerates (up to 30s).
    for (var i = 0; i < 30; i++) {
      final info = await _client.deviceInfo();
      if (info != null && info.length >= 16) {
        if (mounted) {
          setState(() {
            _vendorInfo = info;
            _status = '';
          });
        }
        return;
      }
      await Future.delayed(const Duration(seconds: 1));
    }
    if (mounted) {
      setState(() {
        _status = 'Node not detected. Hold boot button and reset the node.';
      });
    }
  }

  Future<void> _leaveBootloader() async {
    await _client.switchMode(enter: false);
    if (!mounted) return;
    setState(() {
      _inBootloader = false;
      _vendorInfo = null;
    });
  }

  Future<void> _uploadFirmware() async {
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
        messenger.showSnackBar(const SnackBar(content: Text('File is empty')));
        return;
      }

      setState(() {
        _uploading = true;
        _progress = 0;
        _status = 'Writing firmware...';
        _error = null;
      });

      final writeOk = await _client.uploadBinary(
        binary,
        onProgress: (current, total) {
          if (mounted) setState(() => _progress = current / total);
        },
      );

      if (!writeOk) {
        if (mounted) {
          setState(() {
            _uploading = false;
            _status = '';
            _error = 'Firmware write failed';
          });
        }
        return;
      }

      setState(() {
        _verifying = true;
        _progress = 0;
        _status = 'Verifying firmware...';
      });

      final verifyOk = await _client.verifyBinary(
        binary,
        onProgress: (current, total) {
          if (mounted) setState(() => _progress = current / total);
        },
      );

      if (mounted) {
        setState(() {
          _uploading = false;
          _verifying = false;
          _progress = verifyOk ? 1.0 : 0;
          _status =
              verifyOk ? 'Upload complete — reset the node to boot new firmware' : '';
          _error = verifyOk ? null : 'Verification failed — try uploading again';
        });
      }
    } catch (e) {
      if (mounted) {
        setState(() {
          _uploading = false;
          _verifying = false;
          _status = '';
          _error = 'Upload error: $e';
        });
      }
    }
  }

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

  String _formatDeviceType(int type) {
    switch (type) {
      case 0x01:
        return 'Tamu v2.0A';
      case 0x03:
        return 'DAS v0.1';
      default:
        return 'Unknown (0x${type.toRadixString(16)})';
    }
  }

  String _formatSerial(Uint8List info) {
    // Bytes 2..15 = serial number (14 bytes, raw chip ID)
    final sb = StringBuffer();
    for (var i = 2; i < 16; i++) {
      if (i > 2) sb.write(':');
      sb.write(info[i].toRadixString(16).padLeft(2, '0'));
    }
    return sb.toString();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Bootloader'),
        actions: [
          if (_uploading || _verifying)
            const Padding(
              padding: EdgeInsets.all(16),
              child: SizedBox(
                  width: 18,
                  height: 18,
                  child: CircularProgressIndicator(strokeWidth: 2)),
            )
          else
            RefreshButton(
              onRefresh: _checkMode,
              autoActive: false,
              refreshing: false,
              error: _error != null,
              selectedInterval: null,
              onSelectAuto: (_) {},
            ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Card(
            color: kSurfaceAlt,
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(children: [
                    Icon(
                      _inBootloader
                          ? Icons.check_circle
                          : Icons.power_settings_new,
                      color: _inBootloader ? Colors.green : Colors.grey,
                      size: 20,
                    ),
                    const SizedBox(width: 8),
                    Text(
                      _inBootloader ? 'Bootloader Mode Active' : 'Normal Mode',
                      style: const TextStyle(fontWeight: FontWeight.w600),
                    ),
                    const Spacer(),
                    if (_inBootloader)
                      OutlinedButton(
                        onPressed: _leaveBootloader,
                        child: const Text('Leave'),
                      )
                    else
                      FilledButton(
                        onPressed: ConnectionManager.instance.isConnected
                            ? _enterBootloader
                            : null,
                        child: const Text('Enter Bootloader'),
                      ),
                  ]),
                  if (_vendorInfo != null) ...[
                    const Divider(height: 24),
                    Text(
                      'Node: ${_formatDeviceType(_vendorInfo![0] | (_vendorInfo![1] << 8))}',
                      style: Theme.of(context).textTheme.bodySmall,
                    ),
                    Text(
                      'Serial: ${_formatSerial(_vendorInfo!)}',
                      style: Theme.of(context).textTheme.bodySmall,
                    ),
                  ],
                ],
              ),
            ),
          ),
          const SizedBox(height: 12),
          if (_inBootloader) ...[
            Card(
              color: kSurfaceAlt,
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text('Firmware Upload',
                        style: TextStyle(fontWeight: FontWeight.w600)),
                    const SizedBox(height: 12),
                    if (_uploading || _verifying) ...[
                      Text(_status,
                          style: Theme.of(context).textTheme.bodySmall),
                      const SizedBox(height: 8),
                      LinearProgressIndicator(value: _progress),
                      const SizedBox(height: 4),
                      Text('${(_progress * 100).toStringAsFixed(0)}%',
                          style: Theme.of(context).textTheme.bodySmall),
                    ] else ...[
                      if (_error != null)
                        Padding(
                          padding: const EdgeInsets.only(bottom: 8),
                          child: Text(_error!,
                              style: TextStyle(
                                  color: Theme.of(context).colorScheme.error,
                                  fontSize: 13)),
                        ),
                      if (_status.isNotEmpty)
                        Padding(
                          padding: const EdgeInsets.only(bottom: 8),
                          child: Text(_status,
                              style: Theme.of(context).textTheme.bodySmall),
                        ),
                      Align(
                        alignment: Alignment.centerRight,
                        child: FilledButton.icon(
                          icon: const Icon(Icons.system_update_alt, size: 18),
                          label: const Text('Upload Firmware'),
                          onPressed: _vendorInfo != null ? _uploadFirmware : null,
                        ),
                      ),
                    ],
                  ],
                ),
              ),
            ),
          ] else ...[
            Center(
              child: Padding(
                padding: const EdgeInsets.only(top: 48),
                child: Column(
                  children: [
                    Icon(Icons.power_settings_new,
                        size: 64,
                        color: Colors.white.withValues(alpha: 0.3)),
                    const SizedBox(height: 16),
                    Text('Enter bootloader mode to upload firmware',
                        style: Theme.of(context).textTheme.bodyLarge),
                  ],
                ),
              ),
            ),
          ],
        ],
      ),
    );
  }
}
