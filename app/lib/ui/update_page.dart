import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/bootloader_client.dart';
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/transport.dart';
import '../core/notice.dart';
import 'theme.dart';
import 'widgets.dart';

/// Update page (Docs/Services/Bootloader.md): pick a main binary from the host, flash it to a
/// device sitting in bootloader mode, and see the write/verify progress. Doubles as the user
/// guide for entering bootloader mode and for what the progress means.
class UpdatePage extends StatefulWidget {
  const UpdatePage({
    super.key,
    this.isConnected,
    this.clientFactory,
    this.initialImage,
  });

  /// Whether the app has a link, for a test that mounts the page with no manager (the app passes
  /// nothing and the page reads [ConnectionManager.instance]).
  final bool Function()? isConnected;

  /// Builds the bootloader channel; a test supplies a client over its own transport.
  final BootloaderClient Function()? clientFactory;

  /// A firmware image already chosen, for a test that cannot open a file picker.
  final Uint8List? initialImage;

  @override
  State<UpdatePage> createState() => _UpdatePageState();
}

class _UpdatePageState extends State<UpdatePage> {
  final _db = DeviceDatabase.instance;

  Uint8List? _image;
  String _fileName = '';

  bool _probing = false;
  bool _busy = false;
  bool _cancelRequested = false;
  bool? _probeOk;
  BootloaderIdentity? _identity;
  bool _directToCore = false;
  FlashProgress? _progress;
  FlashResult? _result;
  final _log = <String>[];

  ConnectionManager get _link => ConnectionManager.instance;

  @override
  void initState() {
    super.initState();
    final image = widget.initialImage;
    if (image != null) {
      _image = image;
      _fileName = 'image.bin';
    }
  }

  /// Whether there is a link: the injected answer when a test provides one.
  bool get _connected => widget.isConnected?.call() ?? _link.isConnected;

  /// The bootloader channel: the connected core's own USB bootloader when selected, else the
  /// core's Device `0020/0021` passthrough to the node bus.
  BootloaderClient _client() =>
      widget.clientFactory?.call() ?? _defaultClient();

  BootloaderClient _defaultClient() {
    if (_directToCore) {
      final t = ConnectionManager.instance.transport;
      if (t is UsbTransport) {
        // Direct USB: the transport owns the per-write pacing (DirectUsbTransport's
        // writePacing); zero the client's own pacing so a write is not delayed twice.
        return BootloaderClient(
          transport: DirectUsbTransport(t),
          writePacing: Duration.zero,
        );
      }
    }
    return BootloaderClient(coreId: coreId);
  }

  void _addLog(String line) {
    if (!mounted) return;
    setState(() => _log.add(line));
  }

  /// Asks the flash to stop. The run checks this between chunks (and between corrections), so it
  /// ends after the chunk in flight rather than at the next pass.
  void _cancelUpdate() {
    setState(() => _cancelRequested = true);
    _addLog('Stopping after the chunk in flight...');
  }

  Future<void> _chooseFile() async {
    final result = await FilePicker.pickFiles(
      type: FileType.custom,
      allowedExtensions: ['bin', 'img'],
    );
    if (!mounted) return;
    final file = result.singleOrNull;
    if (file == null) return;
    final bytes = await file.readAsBytes();
    if (!mounted) return;
    if (bytes.isEmpty) {
      showNotice('That file is empty');
      return;
    }
    setState(() {
      _image = Uint8List.fromList(bytes);
      _fileName = file.name;
      _result = null;
      _progress = null;
      _probeOk = null;
      _identity = null;
      _log.clear();
    });
  }

  /// Probes with an identify request: only a device in bootloader mode answers, and it replies
  /// with its app-space size and name. The running app waits for the standard sync byte and
  /// ignores the raw 0xCA frame.
  Future<bool> _probe() async {
    if (!_connected) {
      _addLog('No link: connect to the core on the Connection page first.');
      return false;
    }
    setState(() => _probing = true);
    try {
      final id = await _client().identify();
      final ok = id != null;
      setState(() {
        _probeOk = ok;
        _identity = id;
      });
      if (id != null) {
        _addLog(
          'Bootloader identified: ${id.name} - '
          '${_formatBytes(id.appSize)} app space. Ready to update.',
        );
      } else {
        _addLog(
          'No bootloader reply. Put the target in bootloader mode '
          '(hold its button while resetting/powering it on), then retry.',
        );
      }
      return ok;
    } finally {
      if (mounted) setState(() => _probing = false);
    }
  }

  Future<void> _startUpdate() async {
    final image = _image;
    if (image == null) {
      showNotice('Choose a firmware file first');
      return;
    }
    if (!_connected) {
      showNotice('Connect to the core first');
      return;
    }

    setState(() {
      _busy = true;
      _cancelRequested = false;
      _result = null;
      _progress = null;
      _probeOk = null;
      _identity = null;
    });
    _log.clear();
    _addLog('Image "$_fileName" (${_formatBytes(image.length)}).');
    _addLog('Probing for a device in bootloader mode...');

    final client = _client();
    try {
      final id = await client.identify();
      if (id == null) {
        _addLog(
          'No bootloader reply - aborting. Enter bootloader mode and retry.',
        );
        return;
      }
      setState(() => _identity = id);
      if (image.length > id.appSize) {
        _addLog(
          'Image is ${_formatBytes(image.length)}, but ${id.name} has only '
          '${_formatBytes(id.appSize)} of app space - aborting before writing.',
        );
        return;
      }
      _addLog(
        'Bootloader identified: ${id.name} '
        '(${_formatBytes(id.appSize)} app space). Writing '
        '${_formatBytes(image.length)}...',
      );
      final result = await client.flash(
        image,
        maxImageSize: id.appSize,
        deadline: BootloaderClient.flashDeadline,
        isCancelled: () => _cancelRequested,
        onProgress: (p) {
          if (mounted) setState(() => _progress = p);
        },
      );
      setState(() => _result = result);
      _addLog(
        'Verified: ${result.readBacks} read-back(s), '
        '${result.corrections} correction(s). '
        'Power-cycle the device to run the new firmware.',
      );
    } on FlashAborted catch (aborted) {
      // Cancelled, past its deadline, or one of the three device faults (node silent, write
      // refused, image does not read back). Every one of them leaves the node in bootloader mode,
      // where it waits for a power cycle - no packet can reset it - so the stop says both what
      // happened and what the user has to do, and the two sentences differ per fault.
      _addLog('Update stopped (${aborted.fault.label}): ${aborted.message}.');
      _addLog(aborted.remedy);
    } catch (error) {
      _addLog('Update failed: $error');
    } finally {
      if (mounted) {
        setState(() {
          _busy = false;
          _cancelRequested = false;
        });
      }
    }
  }

  static String _formatBytes(int n) {
    if (n < 1024) return '$n B';
    if (n < 1024 * 1024) return '${(n / 1024).toStringAsFixed(1)} KiB';
    return '${(n / (1024 * 1024)).toStringAsFixed(2)} MiB';
  }

  String _phaseLabel(FlashProgress p) => switch (p.phase) {
    FlashPhase.writing => 'Writing',
    FlashPhase.verifying => 'Verifying (read-back)',
    FlashPhase.correcting => 'Correcting',
    FlashPhase.done => 'Done',
  };

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Update'),
        leading: const ShellDrawerButton(),
      ),
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

  Widget _guideCard(BuildContext context) => InfoCard(
    title: 'How to update',
    children: [
      _bullet(context, 'Choose the device\'s main binary (.bin) below.'),
      _bullet(
        context,
        'Put the target in bootloader mode: hold its button while resetting or '
        'powering it on. The DAS shows its white LED; the core shows its red '
        'notification LED.',
      ),
      _bullet(
        context,
        'Tap Update. Every write is acknowledged by the target (it reports the '
        'last successfully written chunk), then the image is read back once. A '
        'chunk that does not read back stops the run - the log names the count, '
        'the offsets and what to do.',
      ),
      _bullet(
        context,
        'Power-cycle the device to run the new firmware. A failed update leaves '
        'it in its bootloader, which only a power cycle leaves; retrying after '
        'that is safe.',
      ),
    ],
  );

  Widget _targetCard(BuildContext context) {
    final core = _db.byId(coreId);
    final connected = _connected;
    return InfoCard(
      title: 'Target',
      children: [
        InfoRow('Link', connected ? 'Connected' : 'Not connected'),
        InfoRow('Core', core?.displayName ?? 'Core 0.1'),
        InfoRow(
          'Bootloader probe',
          _probeOk == null
              ? 'not checked'
              : (_probeOk! ? 'answered' : 'no reply'),
        ),
        if (_identity != null) ...[
          InfoRow('Device', _identity!.name),
          InfoRow('App space', _formatBytes(_identity!.appSize)),
        ],
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
          child: Align(
            alignment: Alignment.centerLeft,
            child: OutlinedButton.icon(
              icon: _probing
                  ? const SizedBox(
                      width: 16,
                      height: 16,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : const Icon(Icons.search),
              label: const Text('Check for bootloader'),
              onPressed: (_probing || _busy || !connected) ? null : _probe,
            ),
          ),
        ),
        SwitchListTile(
          dense: true,
          contentPadding: const EdgeInsets.symmetric(horizontal: 16),
          title: const Text('Update the connected core directly (USB)'),
          subtitle: const Text(
            "Off: relay to a node on the core's bus. On: flash the core itself; "
            "enter its bootloader by holding the button at reset.",
          ),
          value: _directToCore,
          onChanged: _busy ? null : (v) => setState(() => _directToCore = v),
        ),
      ],
    );
  }

  Widget _imageCard(BuildContext context) => InfoCard(
    title: 'Image',
    children: [
      InfoRow('File', _fileName.isEmpty ? 'none chosen' : _fileName),
      InfoRow('Size', _image == null ? '-' : _formatBytes(_image!.length)),
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
    ],
  );

  Widget _actionCard(BuildContext context) {
    final p = _progress;
    final result = _result;
    return InfoCard(
      title: 'Update',
      children: [
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
            child: Text(
              p == null
                  ? 'Starting...'
                  : '${_phaseLabel(p)}  ${p.completed}/${p.total}'
                        '${p.mismatches > 0 ? '  ·  ${p.mismatches} chunk(s) differ' : ''}',
            ),
          ),
        ],
        if (result != null && !_busy)
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
            child: Row(
              children: [
                const Icon(
                  Icons.check_circle,
                  color: Colors.greenAccent,
                  size: 18,
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    'Verified: ${result.readBacks} read-back(s), '
                    '${result.corrections} correction(s)',
                  ),
                ),
              ],
            ),
          ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 10, 16, 6),
          child: Align(
            alignment: Alignment.centerLeft,
            child: Row(
              children: [
                FilledButton.icon(
                  icon: _busy
                      ? const SizedBox(
                          width: 16,
                          height: 16,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : const Icon(Icons.system_update_alt),
                  label: Text(_busy ? 'Updating...' : 'Update'),
                  onPressed: (_busy || _image == null || !_connected)
                      ? null
                      : _startUpdate,
                ),
                // A flash writes ~13,500 chunks and reads them all back; without this the only
                // way out of a run against a node that left bootloader mode was to wait for it.
                // It stops between chunks, so the chunk in flight is still acknowledged.
                if (_busy) ...[
                  const SizedBox(width: 12),
                  OutlinedButton.icon(
                    icon: const Icon(Icons.stop),
                    label: Text(_cancelRequested ? 'Stopping...' : 'Cancel'),
                    onPressed: _cancelRequested ? null : _cancelUpdate,
                  ),
                ],
              ],
            ),
          ),
        ),
      ],
    );
  }

  Widget _logCard(BuildContext context) => InfoCard(
    title: 'Log',
    children: [
      if (_log.isEmpty)
        _bullet(context, 'Nothing yet.')
      else
        for (final line in _log)
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 2, 16, 2),
            child: Text(line, style: Theme.of(context).textTheme.bodySmall),
          ),
    ],
  );

  // --- small helpers ---------------------------------------------------------

  Widget _bullet(BuildContext context, String text) => Padding(
    padding: const EdgeInsets.fromLTRB(16, 3, 16, 3),
    child: Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Padding(
          padding: EdgeInsets.only(top: 5, right: 8),
          child: Icon(Icons.circle, size: 6, color: kOrange),
        ),
        Expanded(child: Text(text)),
      ],
    ),
  );
}
