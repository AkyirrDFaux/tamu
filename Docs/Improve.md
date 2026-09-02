# Improvement suggestions

## Firmware

- **LED display "Keyed Effect" block** documented in `Docs/Modules/LED display.md` (index 3) but not implemented in `Vysi1Display::Render`. Either implement or remove from doc.
- **DAS Button module** (`Docs/Devices.md` "Button | PC0") listed but no firmware exists. Function unspecified — decide before implementing.
- **Script MemWrite to static blocks** cannot set `ScriptUpdated` flag (schema flags are const). Either document the limitation or add a side-table.
- **RAM preload**: scripts always preload to heap at start; the doc's "run from file with read-only pointers" fallback is not implemented.

## App

- **Storage upload**: `StorageClient.writeFile` (CID 7, FRAG stream) works; the Storage page only downloads. Add upload button with file picker.
- **OS notifications** (`"Allow notifications (To OS)"` in Settings): in-app notifications fire, but OS notifications have no consumer. Implement or remove the setting.
- **Router table viewer**: waits on the not-yet-implemented Router firmware service.
- **Script editor**: input/constant defaults edited as hex; no inline constant value in instructions; no State/Type/MathOp predefine presets; MacroCall has no authoring UI.

## Docs

- **Data Formats.md**: broadcast written `0xFFFFFFFF` but ID is 16-bit (code uses `0xFFFF`). Payload len byte = max 255 not 256. `RemoteOrigin` flag (bit 15) missing from flag list. "16bit (4 bit net + 12 bit device)" inconsistent with flat 16-bit addresses used everywhere.
- **Device service.md**: time-sync direction is core->node (doc says "provides time to core"); sample gap 1.5 s vs "few seconds"; core address hard-coded to 1 (undocumented); SNDB Read not-found = empty (undocumented).
- **Script.md**: symbol example chains output into another op (line 51 forbids this); "Create script" returns *assigned* Script ID (1 byte, 0 = failure) — doc says "Success"; input meta carries per-input style byte (5 B/input vs legacy 4 B) — doc's table should mention this; "Read script" streams whole file — block-level patch/diff would avoid full rewrite.
- **Storage.md**: pointer recovery uses LAST valid slot (doc unclear); `Erase` has no default argument.
- **RSBus**: net/device ID split (4+12 bit) unimplemented — flat 16-bit addresses used everywhere. Deferred until Router.
- **Star geometry** ignores PointNumber (identical to Polygon/circle). Decide intent.
- **Texture rendering** blends over existing buffer content; doc says "texture always clears the buffer". Clarify.
- **AccGyr scale factors** (`/209` accel, `/939` gyro) match previously-working driver but aren't the datasheet sensitivities for programmed ranges. Worth calibration pass.

## Resolved

- **SNDB deletion** covered by `Device service.md` CID 14 (ID=0 deletes entry with that SN). No separate CID needed.
- **No per-device max-payload negotiation**: all devices now handle full 276-B payload/288-B frame. Old DAS `-D MAX_PAYLOAD_SIZE=128` removed.
- **FRAG Information is service-specific**: reassembler strips only fixed 4-B FRAG info; stream header (file name / script ID) stripped by each client. Could extract shared helper.
- **Last-fragment padding**: clients trim with known file size. `readFile(size:)` callers must pass size.
