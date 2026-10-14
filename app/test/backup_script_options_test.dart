// The semantic backup form must carry a dropdown input's named choices. They live in the SCR_XXX
// image's UI blob, so with no field for them a capture keeps them only in the file and a restore
// rebuilds an input that renders as nothing at all (B22).
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/types.dart';

void main() {
  test('dropdown choices survive the semantic round trip', () {
    final draft = ScriptDraft(functionName: 'DropFn');
    draft.inputs.add(
      ScriptDraftValue(
        name: 'Mode',
        type: DataType.number,
        spec: const ScriptInputSpec(
          uiType: ScriptUiType.dropdown,
          options: ['Off', 'Low', 'High'],
        ),
      ),
    );
    draft.outputs.add(
      ScriptDraftValue(name: 'Out', type: DataType.number, size: 4),
    );

    final back = BackupScript.fromJson(
      BackupScript.fromDraft(0, draft).toJson(),
    ).toDraft();
    expect(back.inputs.single.spec.options, ['Off', 'Low', 'High']);
  });

  test('an input with no choices writes no options key', () {
    expect(
      const BackupInputSpec(style: 'Number').toJson().containsKey('options'),
      isFalse,
    );
    expect(BackupInputSpec.fromJson(const {'style': 'Dropdown'}).options, isEmpty);
  });
}
