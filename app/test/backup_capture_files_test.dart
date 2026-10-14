import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';

/// B22 (Issues.md): "Include device files" off silently dropped all scripts, which were gathered
/// only from the captured file list. The option governs the raw device files; scripts (semantic)
/// are captured either way.
void main() {
  test('"include device files" off drops raw files but never scripts', () {
    // With the option off, a script is still read; a raw device file is not.
    expect(readCapturedFile('SCR_004', includeFiles: false), isTrue);
    expect(readCapturedFile('notes.txt', includeFiles: false), isFalse);
    expect(readCapturedFile('notes.txt', includeFiles: true), isTrue);

    // Off: an unparsed script keeps its bytes; the raw device files are dropped.
    expect(
      dropCapturedFile(
        'SCR_004',
        includeFiles: false,
        archivedSemantically: false,
      ),
      isFalse,
    );
    expect(
      dropCapturedFile(
        'notes.txt',
        includeFiles: false,
        archivedSemantically: false,
      ),
      isTrue,
    );

    // On: a raw device file is archived; a parsed script's binary is dropped because the restore
    // rebuilds it from the semantic form.
    expect(
      dropCapturedFile(
        'notes.txt',
        includeFiles: true,
        archivedSemantically: false,
      ),
      isFalse,
    );
    expect(
      dropCapturedFile(
        'SCR_004',
        includeFiles: true,
        archivedSemantically: true,
      ),
      isTrue,
    );

    // Device persistence and the subscription table are never carried as raw files.
    expect(
      dropCapturedFile('.SV', includeFiles: true, archivedSemantically: false),
      isTrue,
    );
    expect(
      dropCapturedFile(
        '.SUBREQ',
        includeFiles: true,
        archivedSemantically: false,
      ),
      isTrue,
    );
  });
}
