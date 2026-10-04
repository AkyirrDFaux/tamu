import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/bootloader_client.dart';
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/host_files.dart';
import 'theme.dart';
import 'widgets.dart';

/// Update page (Docs/Services/Bootloader.md): pick a main binary from the host, flash it to a
/// device sitting in bootloader mode, and see the write/verify progress. Doubles as the user
/// guide for entering bootloader mode and for what the progress means.
class UpdatePage extends StatefulWidget {
  const UpdatePage({super.key});

  @override
  State<UpdatePage> createState() => _UpdatePageState();
}

class _UpdatePageState extends State<UpdatePage> {
  final _db = DeviceDatabase.instance;

  Uint8List? _image;
  String _fileName = '';

  bool _probing = false;
  bool _busy = false;
  bool? _probeOk;
  FlashProgress? _progress;
  FlashResult? _result;
  final _log = <String>[];

  ConnectionManager get _link => ConnectionManager.instance;

  void _addLog(String line) {
    if (!mounted) return;
    setState(() => _log.add(line));
  }

  Future<void> _chooseFile() async {
    final result = await FilePicker.pickFiles(
      type: FileType.custom,
      allowedExtensions: ['bin', 'img', 'uf2', 'hex'],
      withData: true,
    );
    if (!mounted) return;
    final file = result?.files.singleOrNull;
    if (file == null) return;
    final bytes = file.bytes ?? readPlatformFile(file.path ?? '');
    if (bytes.isEmpty) {
      showSnack(context, 'That file is empty');
      return;
    }
    setState(() {
      _image = Uint8List.fromList(bytes);
      _fileName = file.name;
      _result = null;
      _progress = null;
      _probeOk = null;
      _log.clear();
    });
  }

  /// Probes offset 0: only a device in bootloader mode answers a read. The running app waits
  /// for the standard sync byte and ignores the raw 0xCA frame.
  Future<bool> _probe() async {
    if (!_link.isConnected) {
      _addLog('No link: connect to the core on the Connection page first.');
      return false;
    }
    setState(() => _probing = true);
    try {
      final data = await BootloaderClient(coreId: coreId).readChunk(0);
      final ok = data != null;
      setState(() => _probeOk = ok);
      _addLog(ok
          ? 'Bootloader answered at offset 0 - ready to update.'
          : 'No bootloader reply. Put the target in bootloader mode '
              '(hold its button while resetting/powering it on), then retry.');
      return ok;
    } finally {
      if (mounted) setState(() => _probing = false);
    }
  }

  Future<void> _startUpdate() async {
    final image = _image;
    if (image == null) {
      showSnack(context, 'Choose a firmware file first');
      return;
    }
    if (!_link.isConnected) {
      showSnack(context, 'Connect to the core first');
      return;
    }

    setState(() {
      _busy = true;
      _result = null;
      _progress = null;
      _probeOk = null;
    });
    _log.clear();
    _addLog('Image "$_fileName" (${_formatBytes(image.length)}).');
    _addLog('Probing for a device in bootloader mode...');

    final client = BootloaderClient(coreId: coreId);
    try {
      if (await client.readChunk(0) == null) {
        _addLog('No bootloader reply - aborting. Enter bootloader mode and retry.');
        return;
      }
      _addLog('Bootloader found. Writing ${_formatBytes(image.length)}...');
      final result = await client.flash(image, onProgress: (p) {
        if (mounted) setState(() => _progress = p);
      });
      setState(() => _result = result);
      if (result.verified) {
        _addLog('Verified in ${result.passes} pass(es), '
            '${result.corrections} correction(s). '
            'Reset/power-cycle the device to run the new firmware.');
      } else {
        _addLog('Not verified after ${result.passes} passes '
            '(${result.corrections} corrections). Check the link and retry.');
      }
    } catch (error) {
      _addLog('Update failed: $error');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  static String _formatBytes(int n) {
    if (n < 1024) return '$n B';
    if (n < 1024 * 1024) return '${(n / 1024).toStringAsFixed(1)} KiB';
    return '${(n / (1024 * 1024)).toStringAsFixed(2)} MiB';
  }

  String _phaseLabel(FlashProgress p) => switch (p.phase) {
        FlashPhase.writing => 'Writing',
        FlashPhase.verifying => 'Verifying (pass ${p.round})',
        FlashPhase.correcting => 'Correcting',
        FlashPhase.done => 'Done',
      };

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Update')),
      body: ListenableBuilder(
        listenable: Listenable.merge([_link, _db]),
        builder: (context, _) => ListView(
          padding: const EdgeInsets.all(12),
          children: [
            _guideCard(context),
            const SizedBox(height: 12),
            _targetCard(context),
            const SizedBox(height: 12),
            _imageCard(context),
            const SizedBox(height: 12),
            _actionCard(context),
            const SizedBox(height: 12),
            _logCard(context),
          ],
        ),
      ),
    );
  }

  // --- sections --------------------------------------------------------------

  Widget _guideCard(BuildContext context) => _card(context, 'How to update', [
        _bullet(context, 'Choose the device\'s main binary (.bin) below.'),
        _bullet(context,
            'Put the target in bootloader mode: hold its button while resetting or '
            'powering it on. The DAS shows its white LED; the core shows its red '
            'notification LED.'),
        _bullet(context,
            'Tap Update. Writes are broadcast on the connected core\'s RSBus, then '
            'read back and corrected until verified. The node gives no write '
            'acknowledgement, so verification is what confirms the image.'),
        _bullet(context,
            'Reset/power-cycle the device to run the new firmware. A failed or '
            'interrupted update is safe to retry.'),
      ]);

  Widget _targetCard(BuildContext context) {
    final core = _db.byId(coreId);
    final connected = _link.isConnected;
    return _card(context, 'Target', [
      _row('Link', connected ? 'Connected' : 'Not connected'),
      _row('Core', core?.displayName ?? 'Core 0.1'),
      _row('Bootloader probe', _probeOk == null
          ? 'not checked'
          : (_probeOk! ? 'answered' : 'no reply')),
      Padding(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
        child: Align(
          alignment: Alignment.centerLeft,
          child: OutlinedButton.icon(
            icon: _probing
                ? const SizedBox(
                    width: 16,
                    height: 16,
                    child: CircularProgressIndicator(strokeWidth: 2))
                : const Icon(Icons.search),
            label: const Text('Check for bootloader'),
            onPressed: (_probing || _busy || !connected) ? null : _probe,
          ),
        ),
      ),
    ]);
  }

  Widget _imageCard(BuildContext context) => _card(context, 'Image', [
        _row('File', _fileName.isEmpty ? 'none chosen' : _fileName),
        _row('Size', _image == null ? '-' : _formatBytes(_image!.length)),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
          child: Align(
            alignment: Alignment.centerLeft,
            child: OutlinedButton.icon(
              icon: const Icon(Icons.folder_open),
              label: const Text('Choose file'),
              onPressed: _busy ? null : _chooseFile,
            ),
          ),
        ),
      ]);

  Widget _actionCard(BuildContext context) {
    final p = _progress;
    final result = _result;
    return _card(context, 'Update', [
      if (_busy) ...[
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
          child: LinearProgressIndicator(
            value: p?.fraction.clamp(0.0, 1.0),
            backgroundColor: kSurface,
            color: kOrange,
          ),
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 6, 16, 0),
          child: Text(p == null
              ? 'Starting...'
              : '${_phaseLabel(p)}  ${p.completed}/${p.total}'
                  '${p.mismatches > 0 ? '  ·  ${p.mismatches} to correct' : ''}'),
        ),
      ],
      if (result != null && !_busy)
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
          child: Row(children: [
            Icon(result.verified ? Icons.check_circle : Icons.error,
                color: result.verified ? Colors.greenAccent : Colors.redAccent,
                size: 18),
            const SizedBox(width: 8),
            Expanded(
              child: Text(result.verified
                  ? 'Verified: ${result.passes} pass(es), '
                      '${result.corrections} correction(s)'
                  : 'Not verified after ${result.passes} passes'),
            ),
          ]),
        ),
      Padding(
        padding: const EdgeInsets.fromLTRB(16, 10, 16, 6),
        child: Align(
          alignment: Alignment.centerLeft,
          child: FilledButton.icon(
            icon: _busy
                ? const SizedBox(
                    width: 16,
                    height: 16,
                    child: CircularProgressIndicator(strokeWidth: 2))
                : const Icon(Icons.system_update_alt),
            label: Text(_busy ? 'Updating...' : 'Update'),
            onPressed: (_busy || _image == null || !_link.isConnected)
                ? null
                : _startUpdate,
          ),
        ),
      ),
    ]);
  }

  Widget _logCard(BuildContext context) => _card(context, 'Log', [
        if (_log.isEmpty)
          _bullet(context, 'Nothing yet.')
        else
          for (final line in _log)
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 2, 16, 2),
              child: Text(line, style: Theme.of(context).textTheme.bodySmall),
            ),
      ]);

  // --- small helpers ---------------------------------------------------------

  Widget _bullet(BuildContext context, String text) => Padding(
        padding: const EdgeInsets.fromLTRB(16, 3, 16, 3),
        child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
          const Padding(
            padding: EdgeInsets.only(top: 5, right: 8),
            child: Icon(Icons.circle, size: 6, color: kOrange),
          ),
          Expanded(child: Text(text)),
        ]),
      );

  Widget _card(BuildContext context, String title, List<Widget> children) {
    return Card(
      color: kSurfaceAlt,
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
              child: Text(title,
                  style: const TextStyle(
                      color: kOrange, fontWeight: FontWeight.w600)),
            ),
            ...children,
          ],
        ),
      ),
    );
  }

  Widget _row(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 3),
      child: Row(children: [
        SizedBox(
            width: 140,
            child:
                Text(label, style: Theme.of(context).textTheme.bodySmall)),
        Expanded(child: Text(value)),
      ]),
    );
  }
}
