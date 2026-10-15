/// System block (type 0, inst 0) model: field/key structure and value formatting
/// (extracted from register_page.dart so the memory views share one definition).
library;

// The field/key names live in core/system_schema.dart; re-export so existing
// `system_block_view.dart` importers keep working.
export '../core/system_schema.dart';

/// Formats one system-block value (special-cases the device type enum, serial
/// number, addresses, timestamps and the 4-byte software version). `field`/`key`
/// let field-specific values (the capabilities bitmask under Device Type) decode.

/// Decodes the capability bitmask (Device Type field, key 1) into readable names.
