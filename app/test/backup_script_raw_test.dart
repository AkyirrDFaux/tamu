import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';

/// B22 (Issues.md): a script whose image fails to parse is captured as a raw binary, then dropped
/// by the restore - which skipped every `kind == 'Script'` file. With no semantic form its bytes
/// are the only copy, so it has to be written back as a raw file.
void main() {
  BackupFile rawScript() => BackupFile(
    name: 'SCR_004',
    kind: 'Script',
    data: base64Encode(const [1, 2, 3, 4]),
  );

  test('a script with no semantic form is restored from its raw bytes', () async {
    final device = BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      blocks: const [],
      scripts: const [], // the image failed to parse, so there is no semantic script
      files: [rawScript()],
    );
    final plan = await buildRestorePlan([device]);
    final files = plan.items
        .where((i) => i.kind == RestoreKind.file)
        .toList();
    expect(files, hasLength(1), reason: 'the raw bytes are the only copy');
    expect(files.single.file!.name, 'SCR_004');
    expect(files.single.file!.bytes, const [1, 2, 3, 4]);

    // A semantic script for the SAME slot supersedes the raw copy (it is rebuilt from
    // `RestoreKind.script`), so the raw file is skipped.
    final withSemantic = BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      blocks: const [],
      scripts: const [
        BackupScript(
          slot: 4,
          functionName: 'Blink',
          properties: [],
          inputs: [],
          outputs: [],
          variables: [],
          constants: [],
          lines: [],
        ),
      ],
      files: [rawScript()],
    );
    expect(restoresAsRawFile(withSemantic, withSemantic.files.single), isFalse);
    // The subscription table always has its own semantic form.
    expect(
      restoresAsRawFile(
        device,
        BackupFile(name: '.SUBREQ', kind: 'Registry backup', data: ''),
      ),
      isFalse,
    );
  });
}
