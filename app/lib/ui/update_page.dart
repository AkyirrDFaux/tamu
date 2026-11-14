import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/bootloader_client.dart';
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/firmware_package.dart';
import '../core/transport.dart';
import '../core/notice.dart';
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
  bool _directToCore = false;
  FlashProgress? _progress;
  FlashResult? _result;
  final _log = <String>[];

  ConnectionManager get _link => ConnectionManager.instance;

  /// The target the chosen route writes; the package's manifest has to name it.
  String get _target => routeTarget(directToCore: _directToCore);

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
  BootloaderClient _client() =>
      widget.clientFactory?.call() ?? _defaultClient();

  BootloaderClient _defaultClient() {
    if (_directToCore) {
      final t = ConnectionManager.instance.transport;
      if (t is UsbTransport) {
        // Direct USB: the raw frames go straight to the core's own bootloader, so the core's
        // passthrough (and its relay) is skipped entirely.
        return BootloaderClient(transport: DirectUsbTransport(t));
      }
    }
    return BootloaderClient(coreId: coreId);
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

  /// Switches the route and re-checks the chosen file against the new target, so a package for
  /// the other route cannot stay selected and be written by mistake.
  void _setRoute(bool directToCore) {
    setState(() => _directToCore = directToCore);
    final bytes = _packageBytes;
    if (bytes != null) _adopt(_fileName, bytes);
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
    final package = _package;
    if (package == null) {
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
        '(${_formatBytes(id.appSize)} app space, ${id.pageSize} B erase pages). '
        'Writing ${_formatBytes(image.length)}: every page is read and compared '
        'first, only the pages that differ are erased and written, and the pages '
        'after the image are blanked. The device\'s whole-space CRC is compared '
        'with the host\'s ${hex32(package.crc32Padded)} only when the image fills '
        'the app space; the client computes the value for the space it writes.',
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
      _addLog('Verified: ${result.note}.');
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
        SwitchListTile(
          dense: true,
          contentPadding: const EdgeInsets.symmetric(horizontal: 16),
          title: const Text('Update the connected core directly (USB)'),
          subtitle: const Text(
            "Off: relay to a node on the core's bus. On: flash the core itself; "
            "enter its bootloader by holding the button at reset.",
          ),
          value: _directToCore,
          onChanged: _busy ? null : _setRoute,
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
                  onPressed: (_busy || _package == null || !_connected)
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
