/// System block (type 0, inst 0) field/key names, re-exported for the pages that import this
/// path.
///
/// The schema itself lives in `core/system_schema.dart`. The value formatting
/// (`formatSystemValue`, with the capability bitmask it decodes) lives in `ui/formatters.dart`;
/// it used to live here, which is why the path still exists.
library;

// The field/key names live in core/system_schema.dart; re-export so existing
// `system_block_view.dart` importers keep working.
export '../core/system_schema.dart';
