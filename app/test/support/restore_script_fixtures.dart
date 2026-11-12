/// The archive, the target and the scripts the three script-restore suites put on them.
///
/// `backup_restore_bringup_test`, `backup_restore_script_slots_test` and
/// `backup_restore_script_files_test` each declared the same three fixtures - a core archive with
/// no blocks, a live core that can run scripts, and a script with no body - and they drifted exactly
/// where it hurts: one archive named the rig's address `39.1`, another the bare id `1`, and only two
/// of the three gave a script its properties. Those differences matter to the assertions, so they
/// are parameters here rather than three copies that disagree.
library;

import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/types.dart';

/// The archive a restore reads: a core, with no blocks, carrying [scripts] and [files].
BackupDevice scriptArchive({
  required int id,
  List<BackupScript> scripts = const [],
  List<BackupFile> files = const [],
}) => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: id,
  name: 'Tamu',
  blocks: const [],
  scripts: scripts,
  files: files,
);

/// The live core a restore writes onto: it can run scripts, and holds no blocks.
LiveDevice scriptTarget({required int id}) => LiveDevice(
  id: id,
  name: 'Tamu',
  capabilities: Capability.scripts,
  blocks: const [],
);

/// A script with a slot, a name and (when a case needs them) properties - and no body at all, so
/// its image is the empty program unless the case builds one.
BackupScript bareScript(
  int slot,
  String name, {
  List<String> properties = const [],
}) => BackupScript(
  slot: slot,
  functionName: name,
  properties: properties,
  inputs: const [],
  outputs: const [],
  variables: const [],
  constants: const [],
  lines: const [],
);
