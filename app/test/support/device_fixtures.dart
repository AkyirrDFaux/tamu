/// The live device that PROVIDES a subscription: one ResistiveMeasure block carrying the
/// measured value at field 3, key 0.
///
/// Three suites (`backup_format`, `backup_restore_requester_prune`, `upload_setup_read_back`) each
/// wrote this device out to give their restore plan a provider row to point at; only the node id,
/// the name and the capability bits differed, because those are what each case asserts about.
/// The block - the thing the subscription's source register names - is the same bytes on every
/// path, so it lives here once.
library;

import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/types.dart';

/// The provider block: a static ResistiveMeasure (type 8) with its measured value at field 3.
LiveBlock providerBlock() => LiveBlock(
  type: 8,
  instance: 0,
  name: 'Resistive measurement',
  isDynamic: false,
  entries: [
    LiveEntry(
      field: 3,
      key: 0,
      fieldName: 'Measured Value',
      keyName: 'Key 0',
      meta: ValueInfo(type: DataType.number.value, size: 4),
    ),
  ],
);

/// The provider node: [providerBlock] under the id, name and capabilities a case needs.
LiveDevice providerDevice({
  required int id,
  String name = 'DAS',
  int capabilities = 0,
}) => LiveDevice(
  id: id,
  name: name,
  capabilities: capabilities,
  blocks: [providerBlock()],
);
