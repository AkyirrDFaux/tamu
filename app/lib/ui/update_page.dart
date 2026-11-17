import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/bootloader_client.dart';
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/firmware_package.dart';
import '../core/notice.dart';
import 'dialogs.dart';
import 'formatters.dart';
import 'theme.dart';
import 'widgets.dart';

/// One file the user picked, before it is checked: the name the picker reported and its bytes.
class PickedFirmwareFile {
  const PickedFirmwareFile(this.name, this.bytes);

  final String name;
  final Uint8List bytes;
}

/// Update page (Docs/Services/Bootloader.md): pick a `.fw.zip` package from the host, check it
/// against its own manifest, flash the image inside it to a device sitting in bootloader mode, and
/// see the write/verify progress. Doubles as the user guide for entering bootloader mode and for
/// what the progress means.
///
/// The page writes nothing from a file it has not checked ([verifyFirmwarePackage]): a bare `.bin`
/// is refused by name, and a package whose manifest disagrees with its bytes never becomes
/// [_package], which is the only source of bytes a flash has.
class UpdatePage extends StatefulWidget {
  const UpdatePage({
    super.key,
    this.isConnected,
    this.clientFactory,
    this.pickFile,
    this.initialPackage,
    this.initialPackageName = 'package.fw.zip',
    this.deviceId,
  });

  /// Whether the app has a link, for a test that mounts the page with no manager (the app passes
  /// nothing and the page reads [ConnectionManager.instance]).
  final bool Function()? isConnected;

  /// Builds the bootloader channel; a test supplies a client over its own transport.
  final BootloaderClient Function()? clientFactory;

  /// Opens the file picker; a test supplies the file instead of the platform dialog.
  final Future<PickedFirmwareFile?> Function()? pickFile;

  /// A package already chosen (the `.fw.zip` bytes), for a test that cannot open a picker.
  final Uint8List? initialPackage;

  /// The name [initialPackage] was picked under.
  final String initialPackageName;

  /// The device ID to target for bootloader operations. When provided, enables better
  /// transport auto-detection for nodes on the bus.
  final int? deviceId;

  @override
  State<UpdatePage> createState() => _UpdatePageState();
}

class _UpdatePageState extends State<UpdatePage> {
  final _db = DeviceDatabase.instance;

  /// The checked package: the page's only source of flashable bytes.
  FirmwarePackage? _package;

  /// The chosen file's bytes, kept so a route change re-checks the same file.
  Uint8List? _packageBytes;

  String _fileName = '';

  /// Why the chosen file was refused. A non-null value always means [_package] is null.
  String? _refusal;

  bool _probing = false;
  bool _busy = false;
  bool _cancelRequested = false;
  bool? _probeOk;
  BootloaderIdentity? _identity;
  FlashProgress? _progress;
  FlashResult? _result;
  final _log = <String>[];

  ConnectionManager get _link => ConnectionManager.instance;

  /// The target the chosen route writes; the package's manifest has to name it.
  String get _target => routeTarget(directToCore: false);

  @override
  void initState() {
    super.initState();
    final bytes = widget.initialPackage;
    if (bytes != null) {
      _adopt(widget.initialPackageName, bytes, announce: false);
    }
  }

  /// Whether there is a link: the injected answer when a test provides one.
  bool get _connected => widget.isConnected?.call() ?? _link.isConnected;

  /// The bootloader channel: the connected core's own USB bootloader when selected, else the
  /// core's Device `0020/0021` passthrough to the node bus.
  /// Kept for tests that supply a clientFactory.
  BootloaderClient _client() =>
      widget.clientFactory?.call() ?? _defaultClient();

  BootloaderClient _defaultClient() {
    // Legacy path for tests; the app now uses auto-detection via _buildClient.
    return BootloaderClient(coreId: coreId);
  }

  /// Builds a BootloaderClient using automatic transport detection.
  /// Returns null if no bootloader transport could be found.
  Future<BootloaderClient?> _buildClient() async {
    if (widget.clientFactory != null) {
      return widget.clientFactory!();
    }
    final transport = await BootloaderClient.detectTransport(
      widget.deviceId ?? 0,
    );
    if (transport == null) return null;
    return BootloaderClient(transport: transport);
  }

  void _addLog(String line) {
    if (!mounted) return;
    setState(() => _log.add(line));
  }

  /// Checks [bytes] against the route's target, then adopts the package or records the refusal.
  ///
  /// This is the gate the page is about: a refusal leaves [_package] null, so nothing can be
  /// written from the file at all. Synchronous - a package's digests are computed over its bytes,
  /// not over a stream - so a pick, a route change and a test all go through one path.
  void _adopt(String name, Uint8List bytes, {bool announce = true}) {
    FirmwarePackage? package;
    String? refusal;
    try {
      package = verifyFirmwarePackage(
        bytes: bytes,
        expectedTarget: _target,
        name: name,
      );
    } on PackageRefused catch (refused) {
      refusal = refused.text;
    }
    void apply() {
      _packageBytes = bytes;
      _fileName = name;
      _package = package;
      _refusal = refusal;
      _result = null;
      _progress = null;
      _probeOk = null;
      _identity = null;
      _log.clear();
      if (package != null) {
        _log.add(
          'Package "$name" for ${package.target} ${package.version}: '
          '${_formatBytes(package.size)} at ${hex32(package.address)}, '
          'crc32_padded ${hex32(package.crc32Padded)}. Checked against its '
          'manifest - nothing written yet.',
        );
      }
    }

    if (announce && mounted) {
      setState(apply);
    } else {
      apply();
    }
  }

  /// Asks the flash to stop. The run checks this between chunks (and before the verification), so
  /// it ends after the chunk in flight rather than at the next pass.
  void _cancelUpdate() {
    setState(() => _cancelRequested = true);
    _addLog('Stopping after the chunk in flight...');
  }

  Future<void> _chooseFile() async {
    final picked = await (widget.pickFile?.call() ?? _pickPackage());
    if (!mounted || picked == null) return;
    if (picked.bytes.isEmpty) {
      showNotice('That file is empty');
      return;
    }
    _adopt(picked.name, picked.bytes);
  }

  /// The platform picker. `.bin` is offered on purpose: a bare image has to be refused with a
  /// sentence that names the package, not filtered out of the dialog without a word.
  Future<PickedFirmwareFile?> _pickPackage() async {
    final result = await FilePicker.pickFiles(
      type: FileType.custom,
      allowedExtensions: ['zip', 'bin'],
    );
    final file = result.singleOrNull;
    if (file == null) return null;
    return PickedFirmwareFile(file.name, await file.readAsBytes());
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
      final client = await _buildClient();
      if (client == null) {
        _addLog(
          'No bootloader transport found. Put the target in bootloader mode '
          'and retry.',
        );
        return false;
      }
      final id = await client.identify();
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
    final package = _package;
    final packageBytes = _packageBytes;
    if (package == null || packageBytes == null) {
      showNotice(_refusal ?? 'Choose a firmware package first');
      return;
    }
    final image = package.image;
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
    _addLog(
      'Package "$_fileName" for ${package.target} ${package.version}: '
      '${_formatBytes(image.length)} at ${hex32(package.address)}.',
    );
    _addLog('Probing for a device in bootloader mode...');

    final client = await _buildClient();
    if (client == null) {
      _addLog(
        'No bootloader transport found. Put the target in bootloader mode '
        'and retry.',
      );
      if (mounted) setState(() => _busy = false);
      return;
    }
    try {
      final result = await client.flashPackage(
        packageBytes,
        expectedTarget: _target,
        name: _fileName,
        deadline: BootloaderClient.flashDeadline,
        isCancelled: () => _cancelRequested,
        onProgress: (p) {
          if (mounted) setState(() => _progress = p);
        },
      );
      // Identify the device to log its details (flashPackage runs identify internally).
      final id = await client.identify();
      if (id != null) setState(() => _identity = id);
      setState(() => _result = result);
      _addLog('Verified: ${result.note}.');
      if (result.version != null) {
        _addLog('Version: ${result.version}${result.buildCommit != null ? ' · ${result.buildCommit}' : ''} (target ${result.target}).');
      }
      _addLog(
        'Power-cycle the device to run the new firmware.',
      );
    } on FlashAborted catch (aborted) {
      // Cancelled, past its deadline, or one of the three device faults (node silent, write
      // refused, image does not read back). Every one of them leaves the node in bootloader mode,
      // where it waits for a power cycle - no packet can reset it - so the stop says both what
      // happened and what the user has to do, and the two sentences differ per fault.
      _addLog('Update stopped (${aborted.fault.label}): ${aborted.message}.');
      _addLog(aborted.remedy);
    } on PackageRefused catch (refused) {
      // Should not happen: the package was already verified in _adopt. But if it does,
      // surface the refusal instead of a generic error.
      _addLog('Package refused: ${refused.text}');
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

  /// Jumps the device from bootloader mode back to its application.
  Future<void> _resetToApp() async {
    final package = _package;
    if (package == null) {
      showNotice('Choose a firmware package first');
      return;
    }
    if (!_connected) {
      showNotice('Connect to the core first');
      return;
    }

    final confirmed = await showTamuDialog<bool>(
      context,
      title: 'Reset to App',
      body: const Text(
        'This will jump the device back to its application. Continue?',
      ),
      actions: tamuDialogActions(
        onCancel: () => Navigator.pop(context, false),
        onConfirm: () => Navigator.pop(context, true),
        confirmLabel: 'Reset',
      ),
    );

    if (confirmed != true) return;

    setState(() => _busy = true);
    _addLog('Jumping device to application mode...');

    try {
      // Use auto-detection to find the right transport
      final transport = await BootloaderClient.detectTransport(
        widget.deviceId ?? 0,
      );
      if (transport == null) {
        _addLog(
          'No bootloader found. Put the target in bootloader mode and retry.',
        );
        return;
      }

      final client = BootloaderClient(transport: transport);
      final ok = await client.jump();
      if (ok) {
        _addLog('Device restarted in app mode');
        showNotice('Device restarted in app mode', tone: NoticeTone.success);
        if (mounted) {
          Navigator.of(context).pop();
        }
      } else {
        _addLog('Jump command failed - no ack from device');
        showNotice('Jump failed: device did not acknowledge');
      }
    } catch (error) {
      _addLog('Reset to app failed: $error');
      showNotice('Reset to app failed: $error');
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
    FlashPhase.comparing => 'Comparing pages',
    FlashPhase.writing => 'Erasing and writing',
    FlashPhase.verifying => 'Verifying (CRC + read-back)',
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
            _packageCard(context),
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
      _bullet(
        context,
        'Choose the device\'s firmware package (.fw.zip) below. A bare '
        '.bin is refused: the package carries the image\'s address and '
        'digests in its manifest, and those are what the flash is checked '
        'against.',
      ),
      _bullet(
        context,
        'The package is checked before anything is written: format, '
        'target, address, size, crc32 and the padded crc32 the device '
        'answers with, and no extra file inside it. A file that fails any '
        'check is refused by name and nothing is sent.',
      ),
      _bullet(
        context,
        'Put the target in bootloader mode: hold its button while resetting '
        'or powering it on. The DAS shows its white LED; the core shows its '
        'red notification LED.',
      ),
      _bullet(
        context,
        'Tap Update. The target is identified first (its app space and its '
        'erase page size), then every page of the image is read and compared: '
        'only the pages that differ are erased and written, and the pages after '
        'the image are blanked. The whole space is then verified twice - the '
        'device\'s own CRC and a full read-back - and up to five correction '
        'rounds repair only the pages the read-back names. A space that still '
        'disagrees stops the run, which names the offsets.',
      ),
      _bullet(
        context,
        'Power-cycle the device to run the new firmware. A failed update '
        'leaves it in its bootloader, which only a power cycle leaves; '
        'retrying after that is safe.',
      ),
    ],
  );

  Widget _targetCard(BuildContext context) {
    final core = _db.byId(coreId);
    final connected = _connected;
    final target = firmwareTargets[_target]!;
    return InfoCard(
      title: 'Target',
      children: [
        InfoRow('Link', connected ? 'Connected' : 'Not connected'),
        InfoRow('Core', core?.displayName ?? 'Core 0.1'),
        InfoRow('Package target', '${target.name} at ${hex32(target.address)}'),
        InfoRow(
          'Bootloader probe',
          _probeOk == null
              ? 'not checked'
              : (_probeOk! ? 'answered' : 'no reply'),
        ),
        if (_identity != null) ...[
          InfoRow('Device', _identity!.name),
          InfoRow('App space', _formatBytes(_identity!.appSize)),
          InfoRow('Erase pages', '${_identity!.pageSize} B'),
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
      ],
    );
  }

  Widget _packageCard(BuildContext context) {
    final package = _package;
    final refusal = _refusal;
    return InfoCard(
      title: 'Package',
      children: [
        InfoRow('File', _fileName.isEmpty ? 'none chosen' : _fileName),
        if (package != null) ...[
          InfoRow('Target', package.target),
          InfoRow('Address', hex32(package.address)),
          InfoRow(
            'Version',
            package.buildCommit.isEmpty
                ? package.version
                : '${package.version} · ${package.buildCommit}',
          ),
          InfoRow(
            'Size',
            '${_formatBytes(package.size)} '
                '(${package.paddedSize} B padded)',
          ),
          InfoRow(
            'crc32_padded',
            '${hex32(package.crc32Padded)} (crc32 ${hex32(package.crc32)})',
          ),
        ] else
          InfoRow(
            'State',
            refusal == null ? 'no package chosen' : 'refused - see below',
          ),
        if (refusal != null)
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 6, 16, 2),
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Icon(Icons.error_outline, size: 18, color: kError),
                const SizedBox(width: 8),
                Expanded(child: Text(refusal)),
              ],
            ),
          ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
          child: Align(
            alignment: Alignment.centerLeft,
            child: OutlinedButton.icon(
              icon: const Icon(Icons.folder_open),
              label: const Text('Choose package'),
              onPressed: _busy ? null : _chooseFile,
            ),
          ),
        ),
      ],
    );
  }

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
                Expanded(child: Text('Verified: ${result.note}')),
              ],
            ),
          ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 10, 16, 6),
          child: Align(
            alignment: Alignment.centerLeft,
            child: Wrap(
              spacing: 12,
              runSpacing: 8,
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
                  onPressed: (_busy || _package == null || !_connected)
                      ? null
                      : _startUpdate,
                ),
                // Reset to App button - available after package is verified
                if (_package != null && !_busy)
                  OutlinedButton.icon(
                    icon: const Icon(Icons.restart_alt),
                    label: const Text('Reset to App'),
                    onPressed: _connected ? _resetToApp : null,
                  ),
                // A flash writes ~13,500 chunks and reads them all back; without this the only
                // way out of a run against a node that left bootloader mode was to wait for it.
                // It stops between chunks, so the chunk in flight is still acknowledged.
                if (_busy) ...[
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
