# Issues

Open items only. The 2026-10-04 per-area audit findings (register, storage, device/log,
subscriptions, tamu, das, bootloader, app, tooling, tests) were fixed and committed; the
remaining low-value follow-ups live in `TODO.md`.

## Doc fact check (2026-10-09)

Every firmware document was checked against `firmware/src`. The documents are the specification, so a
mismatch means one side is wrong, and the two lists below say which. Each item gives the current text,
the evidence, and the proposed replacement. **Doc fixes need approval before they are applied**; code
fixes are work for the beta window. Answer by ID: approve, reject or amend each one.

### A. Doc fixes

**A1 Register: dynamic command IDs.** - **rejected**, the document is correct. The ID cell holds the last digit of the CMD: `0` under the `011x` heading is `0x0110`. Matches the owner's own SNDB edit (`001x`: 10/11/12/13 to 0/1/2/3). The guide's wording is the thing to sharpen, see A57. `Docs/Services/Register.md:102-107` - the ID column reads `0`,
`1`, `2`, `3` under the `011x` heading. Code `Core/Services/RegisterDefs.h:39-44`: `Create = 0x10,
Delete = 0x11, GetName = 0x12, SetName = 0x13`. Replace the four ID cells with `0x10`, `0x11`, `0x12`,
`0x13`.

**A2 Register: instance count.** - **approved and applied**. `Register.md:23-24` - `Instances 0-255` on the Dynamic and Scripts
rows, contradicting the 6-bit `BlockInstance` on line 6. Code `RegisterDefs.h:47-55`: 64 instances per
type, a global index `0..255` over the four banks. Replace the note cell with `Instances 0-63 per type;
global index 0-255 across the four banks`.

**A3 Register: enumerate reply omits the dynamic word form.** - **approved and applied**. `Register.md:58` - `Fragmentation, Block
types + maximum instance for each (uint16) stream`. Code `RegisterEnumerate.h:161-168` packs System and
static types as `10.6` and the dynamic range as `8.8`. Replace with `Fragmentation, one packed word per
present type: (type << 6) | maximum instance for the System and static types, and (bank type << 8) |
highest global index for the dynamic range`.

**A4 Register: Create Dynamic request and reply.** - **revised**. The request gains `Name` (`Index (uint16), Name`, a fixed 16-character field), applied. The response keeps `Success (bool)`: the reply's `BlockIndex` echo is redundant because the host already knows the index it asked for, so B9 drops it from the code rather than documenting it. `Register.md:104` - Request `Index (uint16)`,
Response `Success (bool)`. Code `RegisterPersist.h:104-107` reads an index **and a name**
(`name_len = PayloadBytes - 2`) and replies with a block index. Replace with Request `Index (uint16),
Name`, Response `BlockIndex, Ack`.

**A5 Register: longer strings are truncated, not rejected.** - **revised**. `String` and `Filename` are not variable-length strings: they are fixed-size character fields, typically 8, 16 or 24 bytes. Proposed sentence: `A write of a different type or length fails. String and Filename are fixed-size character fields, typically 8, 16 or 24 bytes: a shorter value is null-padded at the end, and a longer one is cut off.` - **approved and applied, with null padding**; the code's space padding is B10. `Register.md:27` - a write of a different
type or length fails, and `String`/`Filename` may be shorter. Code `MemoryTypes.h:148-178` pads a
shorter one (`:153-162`) and clamps a longer one (`:164-166`, "clamp; memcpy copies Size bytes
anyway"); only a non-string length mismatch fails (`:169`). Append `and may be longer and are truncated
to the field size`.

**A6 Storage: the pointer magic does not exist.** - **approved and applied.** `Storage.md:9` - the example row shows
`0x53451345`. No such constant exists anywhere in the tree. The valid entry is the table's
page-aligned flash offset. Replace the example with `0x00001000`.

**A7 Storage: Filesize is the exact byte count.** - **approved and applied.** `Storage.md:16` and `:19` say the size is "a multiple
of a page". Code `StorageBlockFS.h:449` stores the exact size; only the reservation is page-rounded.
Replace `:16` with `In bytes; the reserved space is rounded up to whole pages` and `:19` with `The start
offset of a file is always page-aligned, and the space reserved for it is rounded up to whole pages; the
recorded size is the exact byte count.`

**A8 Storage: zero-length files are accepted.** - **rejected**, the document is right: a zero-size file is pointless and should not exist. The code accepts one, so this becomes B11. `Storage.md:32,34` - `Size (>0)` and `New Size (>0)`.
Code accepts 0 (`StorageDefs.h:57-58`, no zero check). Drop `(>0)` from both cells.

**A9 Storage: fragmentation comes before the name.** - **approved and applied**, with the name in every fragment rather than the first, so the cells read `Fragmentation, Name, File contents (stream)`. The code carries the name in fragment 0 only, so this becomes B12. `Storage.md:36-37` - Response and Request read
`Name, Fragmentation, File contents (stream)`. Code `Core/Services/Storage.h:89-90` puts the 4-byte frag
info first and the 8-byte name at offset 4. Reorder both cells to `Fragmentation, Name (first
fragment), File contents (stream)`.

**A10 Storage: newest entry wins.** - **approved and applied**, with two additions from the owner: older slots need not be invalidated at all (the newest wins), so B13 drops the invalidation loop, and the page is erased once it is full. `Storage.md:5` - "Its first valid entry is that pointer ... The
block is erased only when the last entry is invalidated." Code `StorageBlockFS.h:40-52,73-78`: the newest
valid slot is the pointer, and the page is erased only when no unused slot remains. Replace with `Its
newest valid entry is that pointer ... The page is erased only when every entry is written.`

**A11 Storage: utility signatures return bool with out-parameters.** - **approved and applied.** `Storage.md:49,51,55` -
`uint32_t FindFiletable()`, `uint32_t FindInFiletable(char[8] Filename)`, `uint32_t
GetEndOfFiletable()`. Code `StorageBlockFS.h:43,98,118` returns `bool` and writes through a pointer.
Replace with `bool FindFiletable(uint32_t* Offset)`, `bool FindInFiletable(char[8] Filename, uint32_t*
Index)`, `bool GetEndOfFiletable(uint32_t* Index)`.

**A12 Storage: main function names and parameter order.** - **deferred** to the code-cleanup backlog: give the storage flash API a namespace or class, and group it in the document. `Storage.md:40-47` - `Read/Write/Erase/Format`
with length before buffer. Code `StorageDefs.h:31-35` uses `Storage_FlashRead(uint32_t offset, void*
data, uint32_t size)`, `Storage_FlashWrite`, `Storage_FlashErase(uint32_t offset, uint32_t size)`,
`Storage_FlashFormat()`. Replace names and put the buffer before the length.

**A13 Storage: grow/shrink is build-conditional.** - **revised**: the table size is fixed per device and the table moves only when it is full. Applied to the document; the code still resizes under `OPTIMIZE_SPEED` and still formats one page, so this becomes B15 and B16. `Storage.md:62` states the 75%/25% grow and shrink
unconditionally. Code `StorageBlockFS.h:266-280` guards it with `#ifdef OPTIMIZE_SPEED`. Prefix `In a
speed build, ` and add `a size build keeps the current size.`

**A14 Storage: allocation starts at a wear cursor.** - **approved and applied** with the owner's wording; the cursor replaces the start page and the wear remark goes. `Storage.md:54` - "starting from a start page".
Code `StorageBlockFS.h:415` scans from `(wear_cursor + scanned) % num_blocks`. Replace with `starting
from an internal wear cursor that rotates allocations across the storage`.

**A15 Storage: Format replies with an empty ack.** - **resolved in code**: it should reply a status, so this becomes B17. `Storage.md:31` - Response `Success (bool)`. Code
`Core/Services/Storage.h:20-21` sends `SendResponse(frame, nullptr, 0)`. Replace with `Ack (empty)`.

**A16 Storage: the reduced file system does not exist.** - **approved and applied**; the reduced filesystem was dropped. `Storage.md:32-35` - the note `not in reduced
file system` is stale; the DAS runs the full FS (`DAS_v0.1/Main.h:36-38`, `Capabilities::StorageFiles`).
Delete the clause from all four rows.

**A17 Storage: size range.** - **approved and applied**; the range reflects the real scope without naming devices. `Storage.md:1` - "from 4 kB to 2 MB". The real regions are 512 B (DAS),
8 kB (Valu) and 948 kB (Tamu), `platformio.ini:97,165,299`. Replace with the measured range or per-device
sizes.

**A18 Script: the `Number` predefine is missing.** - **approved and applied.** `Script.md:43-48` - the Predefine block ends at
`Bool`. Code `ScriptDefs.h:67` defines `SCRIPT_PRE_NUMBER 6`, a 16-bit Q8.8 literal. Append the row
`| | Number | 16-bit Q8.8 fixed-point literal |`.

**A19 Subscriptions: the last value covers more types.** - **approved and applied.** `Subscriptions.md:44` - "for `Number` and
`int32` values only". Code `SubscriptionsProvider.h:171-176` accepts `Number`, `Index` and `Uint32`.
Replace with `for Number, Index and Uint32 values only`.

**A20 Subscriptions: the deadzone covers the integer scalars.** - **revised**: the deadzone takes the value's scalar type - `Number` for a `Number`, `uint32` for `Index` and `Uint32`, since a `Number` deadzone is not cleanly comparable against the integer scalars. Applied to the document; B18 makes the code match. `Subscriptions.md:11` - Deadzone
`Number`, "For Number and Vector". Code `SubscriptionsProvider.h:275-282` gates any 4-byte scalar.
Replace with `For Number, Index, Uint32 and Vector`.

**A21 Script: the list reply is fragmented.** - **approved and applied**, with the count removed from the reply. The code still sends it, so B19 drops it there too. `Script.md:88` - Response `Number of loaded scripts
(uint8), Script File IDs (uint16)`. Code `ScriptRuntime.h:164-180` streams it as fragments. Replace with
`Fragmentation, number of loaded scripts (uint8), Script File IDs (uint16) (stream)`.

**A22 Script: Compose/Extract also handles `String`.** - **approved**, with the element carrying a character type rather than a numeric one, so a string element is distinguishable from a number. Confirmed and applied: `Script.md:70` names the element, `Data Formats.md` gained a `Char` row, and the new enum member is B22. The code typing it `Uint32` is B20. `Script.md:70`. Code `ScriptExec.h:509-538`
includes `DataType::String`. Replace with `Vector, Matrix, Colour or String plus an Index convert to and
from Number or uint8.`

**A23 App Interface: BLE payload is `ATT_MTU - 5`.** - **approved and applied.** `App Interface.md:21` - `uint8[ATT_MTU - 2]`. Code
`Devices/Tamu_v2.0A/AppBLE.h:307`: `chunkCap = budget - 3 - 2`. Replace the size cell with
`uint8[ATT_MTU - 5]`.

**A24 System Block: Discover reply carries a kind tag.** - **rejected**, the document is right: the request/reply distinction is a packet flag and the payload length is in the packet, so the tag is redundant. That makes it B21. `System Block and Device Commands.md:42` -
Response `SN (of node) + ID (from core, assignment)`. Code `Services/Device.h:39,315` prefixes
`0xD1`. Replace with `Kind tag (0xD1) + SN (of node) + ID (from core, assignment)`.

**A25 System Block: App Active has three states.** - **rejected**, the document is right: Legacy BT and WiFi are planned. The enum gains them later; noted in `TODO.md`. `System Block...md:23` - `(No/USB/BLE/Legacy
BT/WiFi)`. Code `Enums.h:34-38`: `None`, `USB`, `BLE`. Replace with `(No/USB/BLE)`.

**A26 System Block: the capability list names the wrong bit.** - **rejected**: every device has storage files, so it is not a capability. The document keeps `Router`, which is reserved for expansion, and the CLI stays deprecated. That leaves B23 (drop `StorageFiles` from the capability sets) and a planned router bit in `TODO.md`. `System Block...md:30-38` - lists
`Router` and omits storage. Code `Enums.h:15` has `StorageFiles = 1u << 5`, set by all three targets, and
no router bit. Replace `- Router` with `- Storage Files`.

**A27 System Block: the CLI is gone.** - **approved and applied.** `System Block...md:55` - "through the app or the CLI". Code
`Tamu_v2.0A/AppUSB.h:8`: "There is no console/REPL: the CLI was removed". Replace with `the core remains
accessible through the app to change its NetID, the issue is logged, and the red LED blinks
periodically.`

**A28 System Block: the re-sync interval is fixed.** - **revised and applied**: the interval is device-dependent on the clock's stability and carries jitter so the reference core is not periodically overwhelmed. The code is a fixed 150 s with no jitter, so B24 adds both. `System Block...md:59` - "at random intervals".
Code `TimeSync.h:23,93`: `DISCOVER_INTERVAL_MS = 150000` with no jitter. Replace with `every 2.5
minutes`.

**A29 System Block: discovery repeats at a fixed interval.** - **rejected**, the document is right: jitter is intended so the core and the network are not overwhelmed. The code sends a fixed 500 ms, so B25 adds the jitter. `System Block...md:47` - "at random
intervals". Code `DAS_v0.1/Main.h:142-152` sleeps a fixed 500 ms. Replace with `every 500 ms`.

**A30 Bootloader: the write reply is a flag.** - **rejected**, the document is right: the reply carries a bool, is-ok or is-not. The code sends flags with no payload, so B26 gives it a bool. `Bootloader.md:52` - Response `Success (bool)`. Code
`Services/Device.h:171-173` sets `FLAG_SUCCESS`/`FLAG_FAIL`. Replace with `Success (flag)`.

**A31 Bootloader: the core lights no LED.** - **addressed in the devices file**, not in the bootloader document, whose intention stands. `Devices.md` records the Tamu board's missing white LED and the DAS's PD0 indicator. `Bootloader.md:3` - "lights the white LED permanently".
Code `Bootloader/CoreBootloader.cpp:104-134` drives no LED; only the DAS does
(`DAS_v0.1/Bootloader.cpp:307-322`). Append `(where fitted)`.

**A32 Log Handler: priority is fixed.** - **rejected**, the document is right: per-log priorities are the intention. Every send goes out at `PRIORITY_ERROR` today, so B27 implements proper priorities. `Log Handler.md:12` - "at the priority that corresponds to it".
Code `Log.h:32` always uses `PRIORITY_ERROR`. Replace with `at the highest priority (errors)`.

**A33 Data Formats: `Effect` is not a type.** - **approved and applied.** `Data Formats.md:26` lists it; the enum
(`Enums.h:59-61`) has `UnknownKeyed`, `Geometry`, `Texture` only, and effects are values continuing the
texture enum. Delete the row and reword line 53 to name the three keyed types, adding `Texture values
name textures and effects.`

**A34 Data Formats: `Name` is not an enum member.** - **revised**: `Name` becomes a real member (B28 at `0x12`), `Deleted` is removed (it is `None`; no code uses it, B29), and `String` is on its way out (B30, four call sites). `Data Formats.md:17`. Code holds a 16-character
name as `String` with size 16 (`RegisterEnumerate.h:37`). Replace the description with `16 characters,
held as a String of 16 bytes; not a distinct enum member.`

**A35 RSBus: the Scripts TrID range is blank.** - **revised**: the range is the remainder, `0x2000-0xEFFF`, now written. The code caps scripts at `0x2FFF`, so B31 extends it. Also found: every piped wikilink inside a table row was splitting its cell (`[[Target|Label]]` needs `\|`); eight of them, in three files, are escaped now. `RSBus and Packets.md:62` - the range cell is empty.
Code `Packet.h:46-47`: `TRID_SCRIPT_BASE 0x2000`, `TRID_SCRIPT_MAX 0x2FFF`. Fill in `0x2000-0x2FFF`.

**A36 Data Formats: `BlockInfo` is a register pointer.** - **approved and applied**, and the note now says the type field addresses static, dynamic and script blocks alike. `Data Formats.md:22` - "A struct for pointing
scripts". Code `Enums.h:57` and `RegisterDefs.h:21`: a 32-bit register pointer, `Type10 | Instance6 |
Field8 | Key8`. Replace with `A 32-bit register pointer: 10-bit type, 6-bit instance, 8-bit field and
8-bit key.`

**A37 RSBus: the TrID counter is 8-bit.** - **rejected**: no service tag belongs in the TrID, so the document stands and the code is wrong. B32 drops the tag from `NextSystemTrid`, which also means replies can no longer route by that byte. `RSBus and Packets.md:60` - `0x0000-0x0FFF, Incrementing,
resets on overflow`. Code `Packet.h:121-125`: the high byte is the service tag, so each service uses
`0x_00-0x_FF`. Append `The counter is 8 bits and wraps; the high byte carries the service tag, so a
reply routes to the right service.`

**A38 Data Formats: `DevType` cites a list that has no numbers.** - **approved and applied**: `Devices.md` now opens with a device-type table (`Unknown 0x00`, `Tamu_v2_0A 0x01`, `Valu_v2_0 0x02`, `DualAnalogSensor 0x03`). `Data Formats.md:21` points at
`Devices.md`, which lists no numbers. Code `Enums.h:3-8`: `Tamu_v2_0A = 0x01`, `Valu_v2_0 = 0x02`,
`DualAnalogSensor = 0x03`. Add the numbers to `Devices.md`.

**A39 LED Display: `-1` is a valid sentinel.** - **approved and applied**. `LED Display.md:9` - "Index of the block containing
shapes, textures and effects; -1 is invalid". Code `Vysi1Layout.h:76` (`-1 = none`) and
`Vysi1Render.h:351` (`if (Per.RenderBlock < 0 ...) return;`). Replace the note with `-1 renders
nothing`.

**A40 LED Display: texture Size takes a `Number` only.** - **rejected**: the document keeps both types, so the code must accept a `Vector<2>` for the texture `Size` (B33). `LED Display.md:65` - `Vector<2>` or
`Number`. Code `Vysi1Render.h:180` reads a `Number` and silently defaults on a type mismatch. Replace the
type cell with `Number`.

**A41 LED Display: `Angles` takes a `Number` only.** - **approved and applied**: the type cell is `Number`. `LED Display.md:35` - `Number` or `Vector`. Code
`Vysi1Render.h:48` reads a `Number`; no `Vector` path exists. Replace the type cell with `Number`.

**A42 LED Display: `Bitmap` is not in the enum.** - **approved, reserve the value**: the bullet stays and the code reserves a texture slot for `Bitmap` (B34). Putting it after the effects, `Bitmap = 8`, keeps `InvertColour`..`Brightness` where they are. `LED Display.md:78` lists `- Bitmap (TODO)` between
textures and effects, which shifts the effect numbering. Code `Render.h:72-82` has `GradientCircular =
3` then the effects from `4`. Delete the bullet or move it after the effect list.

**A43 LED Display: `Mesh` draws nothing.** - **approved and applied** (`- Mesh (TODO)`). `LED Display.md:56` lists `- Mesh`. Code `Render.h:42` has
the value but `GeometryMath.h:119-228` has no case, so it falls through to `default: return 0`. Mark it
`- Mesh (TODO)`.

**A44 LED Display: `Point coordinates` is never read.** - **approved and applied** (the note reads `TODO`). `LED Display.md:37` - key 10. Code holds
`PointCoordinates = 10` in `Render.h:23`, but `RenderGeometryField` never reads it. Mark the note
`TODO`.

**A45 LED Display: two Triangle bullets, one enum value.** - **approved and applied**: one bullet, `- Triangle: isosceles (angle and side length)`. The code's equilateral branch becomes a special case of the same shape, so B35 removes it. `LED Display.md:52-53`. Code `Render.h:39`
has one `Triangle = 9` with two modes (`GeometryMath.h:76`). Replace both bullets with `- Triangle:
equilateral (size only) or isosceles (angle and side length)`.

**A46 LED Display: the reserved key 0 is missing from both dictionaries.** - **rejected**: key 0 carries the dictionary's type rather than being a key, so the tables correctly start at 1. The code comment calling it "reserved" is what invited the misreading; B36 rewords it. `LED Display.md:26-38` and
`:61-69` start at key 1. Code `Render.h:13,60` reserves key 0 as the dictionary marker. Add a first row
to each table, `| Dictionary | 0 | DataType::Geometry | Reserved marker; holds no value |`.

**A47 LED Display: the repeated sentence.** - **approved and applied.** `LED Display.md:71` repeats "Not every shape interacts with
every parameter." under the texture table, where it refers to textures and effects. Reword.

**A48 Measurement: the trigger flag.** - **revised and applied**: both rows carry `P, (TR)`, and the Sampling Rate note explains the write-time clamp. The schema's field flags still lack the trigger bit, which is B38. - **revised and applied** on the Sampling Rate row, whose note now says the trigger clamps the stored value to the applied one. The same trigger serves the Filter Coefficient (both fields are clamped at write time, `Measuring.h:60-74`), so that row is missing its `(TR)` marker - either it gains one or the code drops the field-2 trigger. `Measurement.md:7` - `Sampling Rate` flags `P, (TR)`. Code
(`DAS_v0.1/Measuring.h:46,80-83`, `Valu_v2.0/Measuring.h:47,79-82`) sets only `ValuePersistent` and
registers the trigger out of band; both boards also trigger field 2, listed as bare `P`. Replace the
flags with `P` on both rows, or set `ValueTrigger` in code.

**A49 Devices: Tamu I2C pull-ups.** - **rejected**: the board does carry the 4k7 pull-ups and the internal ones stay enabled, so the document is right. The driver's comment claiming there are none is what is wrong; B37 fixes it. `Devices.md:9` - "with 4k7 pull-ups". Code
`Tamu_v2.0A/AccGyr.h:139-150`: "this board has no external pull-ups on SDA/SCL", and enables internal
pull-ups. Replace the note with `internal pull-ups` if the board has none, or fix the code if it does.

**A50 Devices: Valu declares an LED display it does not implement.** - **rejected**: `Vysi` is an instance of the LED display block, and the LED driver is now generic, so it also serves LED strips. The document is right. `Devices.md:79` - `- LED Display,
2 instances`. Code `Valu_v2.0/Main.h:80-89` registers no LED display and no strip driver exists. Mark it
`(TODO)` or implement it.

**A51 Devices: table size has no firmware counterpart.** `Devices.md:30` and `:87` - `4 pages` and
`2 pages`. Code `StorageBlockFS.h:586-597` formats one page of table and grows it only in a speed
build. Replace with `1 page, grown and shrunk to stay between 25% and 75% full` (Tamu) and `1 page`
(Valu).

**A52 Devices: Tamu storage size.** - **approved and applied**: `partitions.csv` reserves `storage` at `0xED000`, 970752 bytes, 948.0 kB, so the row now reads `Storage: 948 kB`. `Devices.md:31` - "a lot (MBs)". Code `platformio.ini:165`:
`STORAGE_FLASH_SIZE=0xED000`, 948 kB. Replace with `948 kB`.

**A53 Current Setup: the script output names.** - **approved and applied** (`offset L`, `offset R`). `Current Setup v3.md:34-35` - `Position L`, `Position
R`. Code `app/test/current_setup_scripts.dart:256-257`: `offset L`, `offset R`. Replace both.

**A54 Current Setup: the script input labels.** `Current Setup v3.md:39,40,52` - `Delay between
blinks`, `Movement time in each direction`, `Switch between auto and manual`. Code
`current_setup_scripts.dart:340-341,127`: `Blink delay`, `Movement time`, `Manual mode`. Replace all
three.

**A55 Current Setup: the brightness cap is reached at 8.8k lux.** `Current Setup v3.md:48` - the last
column reads `>10k Lux`. Code `app/test/current_setup.dart:170-176`: `luxSpan = 8850`. Replace the column
with `>8.8k Lux` (only if 10k is not deliberate).

**A57 Command ID cells are prefix-less.** - **replaced**. The convention is that the heading carries every digit but the last and the cell holds the final one, so no prefix belongs in an ID cell. Instead, the style guide's line reads `the table's ID cell holds the low part, in hex`, which invited the misreading. Proposed: `The leading digits are the high part of the CMD and the ID cell holds the final digit, in hex.` Every ID cell in the service documents is written without
the `0x` prefix - `| Discover | 0 |`, `| Enumerate fields | 1 |`. Values below `0x10` read
unambiguously, but the dynamic table's fix (A1) introduces `0x10`..`0x13`. Either normalise every ID
cell to prefixed uppercase hex, or record the exception in the style guide.

**A56 Current Setup: blink is forced on a shape change only.** `Current Setup v3.md:58` - "Forces a
blink when the pupil changes". Code `current_setup_scripts.dart:686-689` forces it only when the shape
class changes; a lid-only change does not blink. Replace with `when the pupil shape changes`.

### B. Code fixes (the document is the specification)

**B1 Router and branch broadcast are documented and unimplemented.** `RSBus and Packets.md:3-7` and
`Data Formats.md:37` (`0x3FE`) describe a router tree; `Dispatcher.h:36-40` says the multi-bus topology
is not implemented and `Docs/Services/Router.md` is a stub.

**B2 Valu's LED display.** A50 above: two instances are declared, none implemented.

**B3 Texture `Size` and Geometry `Angles` reject the `Vector` the documents allow** (A40, A41), and
`Bitmap` has no slot (A42). Either the dictionary format is trimmed to the code or the code catches
up.

**B4 Measurement fields 0 and 2 have a trigger function but no `ValueTrigger` flag** (A48), while
`Register.md:52` makes that flag the gate.

**B5 System block field 8.1 has no code.** `System Block...md:24` documents `WiFi connection
information` at 8.1; there is no WiFi code in the firmware.

**B6 The UDP framing has no code.** `App Interface.md:25-27` documents a UDP packet; the firmware
implements USB and BLE only.

**B7 SNDB does not store other cores or filter by net.** `System Block...md:65,67` describes storing
other cores by `NetID.1` and excluding foreign nets; `SNDB.h` stores device pairs only, and the core adds
itself as device 1 with no net.

**B8 Tamu is built as Core only.**

**A59 Storage: `Storage_FlashInit` is missing from the main functions.** The document lists four (`Docs/Services/Storage.md:40-47`); the code declares five, the extra being `bool Storage_FlashInit();` ("find/open the storage partition", `StorageDefs.h:31`). Add it to the list.

**B38 The measurement field flags lack the trigger bit.** From A48: `Register.md:52` makes the flag the gate, but `ResistiveMeas_Entries` (`DAS_v0.1/Measuring.h:46-48`, `Valu_v2.0/Measuring.h:47-49`) carries only `ValuePersistent` while `ResistiveMeas_Triggers` (`:80-81`) registers functions for fields 0 and 2. Set the flag on those two entries.

**B36 The dictionary marker is not "reserved".** From A46: key 0 carries the dictionary's type, so the comment `reserved: the Geometry dictionary marker` in `Render.h:13` and `:60` should say so.

**B37 The I2C pull-up comment is wrong.** From A49: `Tamu_v2.0A/AccGyr.h:139-141` says the board has no external pull-ups on SDA/SCL. It has 4k7 ones; the internal pull-ups stay enabled as well, so only the comment changes.

**B34 Reserve a texture slot for `Bitmap`.** From A42: the document lists `Bitmap` and `Textures2D` (`Render.h:72-82`) has no slot - `GradientCircular = 3` runs straight into the effects at `4`. Reserve a value; placing it after the effects (`Bitmap = 8`) keeps the effect numbers stable.

**B35 Remove the equilateral triangle path.** From A45: the shape is isosceles overall. `GeometryMath.h:76` draws equilateral by default and switches to isosceles when `Angles` is set, so dropping it needs a defined default angle - 0 would give a degenerate triangle.

**B33 Texture `Size` ignores a `Vector<2>`.** From A40: the document allows a `Vector<2>` or a `Number` for the texture `Size`, but `Vysi1Render.h:180` reads a `Number` and silently takes the default on a mismatch. Accept the vector as Geometry's `Size` does (`:59`).

**B28 `DataType::Name`.** From A34: add the member (`0x12`, next after `Char`) and use it for the 16-byte Name field (`RegisterEnumerate.h:37`) instead of `String`.

**B29 `DataType::Deleted` is dead.** From A34: nothing in the firmware or app references it; the doc row is gone, so remove the enum member too.

**B30 `DataType::String` is on its way out.** From A34: four call sites - `RegisterEnumerate.h:37` (the Name field), `ScriptExec.h:533` (a string element), and the `String || Filename` checks in `ScriptProgram.h` and `MemoryTypes.h:151`. Migrate them to `Name`/`Filename` and drop the member; the script path needs a decision on what a string value's type becomes.

**B31 The scripts' TrID range is capped at `0x2FFF`.** From A35: the document gives scripts the remainder `0x2000-0xEFFF`; `Packet.h:47` caps it at `0x2FFF`.

**B32 The TrID carries a service tag.** From A37: `Packet.h:117-125` puts the service type in the high byte of every system TrID so an echoed reply routes back to the right service. No tag belongs there; the counter should span the range, and reply routing needs another mechanism.

**B26 The passthrough write reply carries flags, not a bool.** From A30: `Device.h:171-173` replies `FLAG_TYPE | FLAG_START | FLAG_STOP | (ok ? FLAG_SUCCESS : FLAG_FAIL)` with no payload. The reply should carry a bool in the payload as the document says.

**B27 Log priorities are not implemented.** From A32: every report goes out at `PRIORITY_ERROR` (`Core/Functions/Log.h:32`, "Errors are the highest class"). Give each log its proper priority, `PRIORITY_LOG` being the lowest (`Packet.h:32-38`).

**B22 The `DataType` enum gains `Char`.** From A22: a string element needs a character type, and the enum has none. The next free code below the keyed block is `0x11` (`BlockInfo` is `0x10`); the document's row already assumes it.

**B23 `StorageFiles` is not a capability.** From A26: every device has storage files, so the bit says nothing. It is set by all three targets (`Tamu_v2.0A/Main.h:35`, `DAS_v0.1/Main.h:38`, `Valu_v2.0/Main.h:38`) and can come off; the bit itself can stay reserved.

**B24 The core re-sync has no jitter and no device dependence.** From A28: `TimeSync.h:23` is a fixed `DISCOVER_INTERVAL_MS = 150000` scheduled with no variation (`:93`). The interval should follow the device's clock stability and be jittered so the reference core is not overwhelmed.

**B25 The discovery repeat has no jitter.** From A29: `DAS_v0.1/Main.h:142-152` re-broadcasts every 500 ms with no variation. Add jitter so the core and the network are not overwhelmed by simultaneous discovery. The same loop exists on the other Node targets.

**B20 A `String` element is typed `Uint32`.** From A22: `ScriptExec.h:533-538` returns a byte element for `DataType::String` but sets `*elemType = DataType::Uint32`, the same numeric type the `Colour` branch uses. A string element should carry a character type so it stays distinct from the numerics.

**B21 The Discover reply prefixes a kind tag.** From A24: the tag is redundant. `Device.h:314-321` writes `response[0] = DEVICE_REPLY_KIND_ASSIGN` (`0xD1`) and the matching decode at `:216`, `:225` keys off it; the request/reply flag and the packet's payload length already disambiguate. Drop it and update the app's SNDB decoder.

**B18 The deadzone is always a `Number`.** From A20: `SubscriptionsDeltaHash(const FieldResult &fr, Number deadzone)` (`SubscriptionsProvider.h:119`) reads `int32_t dz = deadzone.Value;` (`:127`) for every scalar, so an `Index` or `Uint32` value is gated against a `Number` deadzone. Take the deadzone as the value's scalar type instead.

**B19 The script list reply carries a count byte.** From A21: the count goes. `ScriptRuntime.h:167-170` builds `content[0] = n` ahead of the ids; drop it and let the payload length carry the count, then update the app's reader.

**B17 Format Filesystem replies with no payload.** From A15: it should reply a status. `Storage.h:19-21` sends `SendResponse(frame, nullptr, 0)` where every other command in that table replies through the shared status helper.

**B13 The pointer slots need no invalidation.** From A10: the newest valid slot wins, so the loop that zeroes the earlier slots (`StorageBlockFS.h:84-87`) and the `0x00000000` marker it writes are unnecessary.

**B15 `MoveFiletable` resizes the table.** From A13: the table size is fixed per device and the table moves only when it is full, so the `OPTIMIZE_SPEED` grow/shrink arithmetic goes (`StorageBlockFS.h:262-280`).

**B16 The table is formatted at a fixed one page.** From A13 and A51: `Format()` sets `entry0.size = PAGE_SIZE` (`StorageBlockFS.h:586-597`) and nothing grows it once B15 lands, so the per-device table size needs a constant the device declares (`Devices.md:37` says 4 pages for Tamu, `:63` one page for Valu).

**B11 Create and Resize File accept a zero size.** From A8: a zero-size file should not exist. `BlocksForSize(0)` divides to zero and is forced to one block (`StorageDefs.h:56-57`), and neither path rejects 0 (`StorageBlockFS.h:443`, `:481-489`). Reject size 0 with a failure status.

**B12 The file name travels in the first fragment only.** From A9: the name belongs in every fragment. The write path reads the name from fragment 0 and contents-only afterwards (`Storage.h:115-128`), and the read response does the same (`Storage.h:89-90`). Carry the name in all fragments, at the cost of 8 payload bytes per fragment, and update the app's file transfer to match.

**B10 Character fields are padded with spaces, not nulls.** A5's ruling makes the pad byte a null. The code pads with spaces in the Register path (`MemoryTypes.h:159`, `memset(pad_buf, ' ', ...)`) and the storage file-name path (`StorageDefs.h:86-97`; `TODO.md:346`), and every device Name field is commented as space-padded (`Tamu_v2.0A/Main.h:77`, `DAS_v0.1/Main.h:68`, `Valu_v2.0/Main.h:70`). The app pads the same way (`register_client.dart` `_padBlockName`, `device_backup.dart:28`, `current_setup.dart:411`), and the HIL asserts it: `hil_script_test.dart:253` fails with "short string not space-padded", plus `register_client_dynamic_test.dart:36`, `tamu_hardware_verification_test.dart:280` and `hil_backup_test.dart:197`. Making the pad a null is a wire- and storage-format change across firmware, app and HIL.

**A58 `String` and `Filename` descriptions in Data Formats.** - **resolved by A34**: `String` is gone, so only `Filename` and `Name` remain as the fixed-size character fields. A consequence of A5: if those types are only ever fixed-size character fields, `Data Formats.md` should say so on both rows rather than describing them as variable-length - null-padded, 8, 16 or 24 bytes.

**B9 Create Dynamic replies with a redundant block index.** `RegisterPersist.h:107` replies `SendBlockIndexAck(frame, index)` - a 3-byte `BlockIndex` echo plus a 1-byte ack - while Delete and Set Name reply a plain status (`:115`, `:131`). The host already knows the index it sent (the app computes it, `register_client.dart:421-431`). Reply a plain status and drop the echo, then have the app use the index it sent instead of `reply[0]` (`register_client.dart:437-439`). `Devices.md:4` says "Core and Node"; `Tamu_v2.0A/Main.h:32-38`
declares `Capabilities::Core` and no node build exists.

### C. Checked and correct

The command ID table matches every service and the code constants. The Bootloader packet layouts, the
Log struct and database entries, the USB framing and CRC coverage, the App TrID range
`0xF000-0xFFFF`, the Register field sizes and version packing, the subscription record sizes, the script
header and block order, the symbol layout, the Acc & Gyr block, the Buttons/LED blocks, the Fan block,
every device pin assignment, and the DAS/Valu storage geometry all verify against the code.

### D. Unverifiable from code

`RSBus and Packets.md:1` supply voltage and USB PD (hardware), `Data Formats.md:7` "14 byte UUID"
(14 bytes confirmed, UUID loose), `Data Formats.md:53` the generic dictionary marker encoding,
`Register.md:34` the Internal/External Access columns, `Register.md:48` "sorting by the whole uint32",
`Storage.md:86-96` the dynamic block descriptor ordering (runtime only), and the Valu feature rows that
are hardware-only.

## Open (2026-10-09)

- **Doc vs code: the `DataType` enum.** `Docs/Data Formats.md` now carries the values, read from
  `firmware/src/Core/Types/Enums.h:40-62`. Three deltas came out of that comparison:
  - **`Name` (16 characters) has no `DataType` member.** The doc defines it as a reusable type; the
    enum has no such entry, so a 16-character name is a `String` or `Filename` in code. Either the
    enum gains a member or the doc drops the type.
  - **`Effect` is documented but absent from the enum**, which has `Geometry` (0x101) and `Texture`
    (0x102) only. `Docs/Modules and Blocks/LED Display.md` specifies effects as well.
  - **Three enum members were undocumented:** `Deleted` (0x0D), `Uint32` (0x0E) and `DevType` (0x0F).
    They are in the doc table now, with descriptions inferred from their names - confirm those.
  - Naming: the doc used `Serial Number`, `ID` and `Generic dictionary` where the enum says `SN`, `Id`
    and `UnknownKeyed`. The table uses the enum names for traceability.

## Open (2026-10-06)

- **Valu v2 bootloader: a sector erase does not take effect.** Full status and measurements in
  `TODO.md`. It is **silent** - the controller sets `EOP` and reports no write-protection error
  while the flash content is unchanged - so the app-facing consequence would be update failures
  that look like nothing happened. Do not build Valu v2 features that depend on re-flashing the
  board until this is resolved.
- **The Valu bootloader can only ever be replaced over the ROM bootloader.** Our own bootloader
  refuses writes below `APP_BASE` by design, and this board has no SWD header, so every bootloader
  iteration needs an ISP session (`wchisp-nightly`, one command per USB reset). Worth keeping in
  mind when deciding how much logic belongs in the bootloader rather than the app.
- **`valu_upload.py` drives `wlink` over SWD, which this board cannot do.** The ISP recipe that
  actually works (USB port reset, then a single `wchisp-nightly` invocation) needs folding into it
  as an `--isp` mode so `./upload.sh boot valu` matches reality.

## Code decisions needed (2026-10-04 docs-conformance sweep)
- **Register write of a longer String/Filename.** `Register.md` says a longer value fails (only a
  shorter one is allowed, space-padded); `MemoryTypes.h` clamps it instead. The app and the "System
  Name clamps to 16 bytes" HIL rely on the clamp. Parked (minor detail).

## Valu v2.0 app (2026-10-06)
- **Resistive measurement reference resistor is undefined.** `Docs/Devices.md` gives three ADC
  inputs (PA6/PA1/PA0) but says "Reference resistor not defined." With no reference and no range
  selectors, the resistance/LDR/NTC transforms in `Devices/Valu_v2.0/MeasuringRun.h` use a
  placeholder `VALU_MEAS_REF_KOHM` (10 kOhm); raw and voltage measurements do not depend on it.
  Needs a hardware value before a resistance-reading module is meaningful.
- **"USB Bootloader" and "Script" services have no capability-bit mapping.** `Docs/Devices.md`
  lists the Valu's services as Mandatory + USB Bootloader + App interface + Dynamic memory +
  Script. `Capabilities::` (Core/Types/Enums.h) has bits for Node/AppInterface/DynamicMemory/
  Scripts/StorageFeatures/Subscriptions but **none for a bootloader** - the code (and the
  bootloader itself) already treats that as "no capability bit" (`TODO.md` Decisions). The Valu
  app advertises `Node | StorageFiles | AppInterface | DynamicMemory | Scripts`.
- **Valu modules not implemented in this pass.** `Docs/Devices.md` also lists "LED Display x2"
  (needs the Vysi1 render/layout stack) and "OLED Display (TODO)" for the Valu. The app implements
  the LED-Button, the three buttons, the one fan output and the three resistive channels (the
  peripheral set the task specified); the two display modules are not built.
- **`MAX_SCRIPTS` is reduced to 16 on the Valu.** The Script service's loaded-script registry is
  `LoadedScript scriptRegistry[256]` (~38 KB) in `ScriptDefs.h`; the CH32V203's 20 KB cannot hold
  it, so the registry is now a `#ifndef`-guarded build knob and the Valu sets `-D MAX_SCRIPTS=16`.
  Higher Scripts-range indices report as unloaded.

## Docs decisions needed (parked unless noted)
- **OS notifications.** `Docs/App/Settings.md` lists "Allow notifications (To OS)" with per-event
  selection; the app persists `notifyOs`/`osEvents`/`suppressOsWhenOpen` but only delivers in-app
  notifications (`app/lib/core/notifications.dart`). Implement OS delivery or mark the setting
  pending.
- **Script UI info format.** `Script.md` describes the UI info as names + per-input limits/UI type;
  the app writes **version 2** with a per-input label list (the custom-enum case). v1 is no longer
  supported, so an old backup's script names fall back to the file name until re-saved - document
  the format.
- **Pre-rotated Position convention.** The renderer samples the geometry/texture masks *forward*,
  so a shape's centre lands at `-L^-1 * t`; both writers (`ScriptExecTransform` and the app's
  `Transform23`) store `t' = L * t` to keep the centre at `-t` under rotation. `LED display.md`
  describes Position as a plain 2x3 matrix - document the convention.
- **Cross-script macro calls.** `Script.md` names "Macro call" but gives no opcode, boundary rule
  or argument passing. The VM runs **one script per tick** and every wait state lives on the
  callee, so it needs a call stack of `(script, line)` pairs and a tick loop that resumes whichever
  script is waiting. Proposal to confirm: a `Call script` flow op with operands
  `(loaded script id, entry line)`, blocking by construction, values exchanged through registers.
- **Script CID 0 lists file IDs, not loaded slots.** CID 1 takes a separate loaded id, so CID 0's
  list is not slot-addressable; the app tracks file->slot and falls back to the file==slot
  convention. Reporting the loaded **slots** (or a slot per entry) would remove the guesswork.
- **Dynamic descriptor doc omits `Name`/`generation`/`present`** (deferred).
- **Dynamic 8.8 enumerate encoding** is only in code comments (`RegisterEnumerate.h`), not in
  `Register.md`.
- **Bootloader LED entry.** `Bootloader.md` (white LED) vs `Devices.md` (core white LED "missing
  hardware") vs the app (red LED). Code drives no LED in the core bootloader.
- **Device-type numbering is undocumented.** `Docs/Data Formats.md` and `System Block and Device
  Commands.md` name the System `Device Type` field but give no numeric table. `Enums.h` /
  `types.dart` assign `Tamu_v2_0A = 0x01`, `Valu_v2_0 = 0x02`, `DualAnalogSensor = 0x03` as a code
  convention (0x02 carried over from the pre-restructure tree) - confirm or document it.

### Docs-conformance sweep wording (2026-10-04)
- **Documented but not implemented** (future/planned, `Plan.md`): WiFi `App Active` values + System
  field 8.1 SSID/Password; Router capability + service (stub only); branch-broadcast address
  `0x3FE`; UDP app transport; the `Mesh` LED-display shape; the `Effect` data
  type. Code-only types (`Deleted`, `Uint32`, `DevType`, `UnknownKeyed`) are undocumented. (The
  **Valu v2.0 app** is now implemented - `[env:Valu_v2_0]`, `Devices/Valu_v2.0/`; its LED-Display x2
  and OLED modules remain unbuilt, see the Valu section above.)
- **Register.md**: dynamic Create request also carries a 16-char name (doc lists only `Index`), and
  the response is `BlockIndex(5)+ack` (doc says `Success`); System struct members are shown as keyed
  positions but only the whole struct is exposed at key 0.
- **Storage.md**: CID 5/6 payload order is frag-info first with the name at +4 (not Name first);
  a file's size is the exact byte size (only allocation is page-aligned); the pointer page's
  *newest* valid slot wins; `MoveFiletable` grow/shrink is `OPTIMIZE_SPEED`-only; utility signatures
  are `bool` + out-params; the flash API is `Storage_FlashX`; size 0 is accepted; `FindSpace` scans
  from an internal wear cursor.
- **Script.md**: the leading varSpace word is a vestigial IC slot; the symbol-subtype list omits
  `Number`; no size table (offsets are prefix sums); CID 5 truncates to 112 B; CID 6 takes a line
  index, not an instruction counter.
- **App Interface.md**: the BLE payload cap is MTU-5, not MTU-2.
- **App docs** were synced to the app on 2026-10-07 (the Update tab, the Connection autoconnect
  toggle, the Subscriptions viewer, the actual Devices graph layout and the Device view data
  source/capability wording are now documented). Remaining item outside `Docs/App`: `Current setup
  v3` lux cap is ~8.85k (not 10k) and the fan is not connected.
- **RSBus/Packets.md**: the Script TRID range is unspecified and the System/Log counter is 8-bit
  (within range) not 12-bit.
- **Subscriptions.md**: the get-subscriptions stream starts with an undocumented count byte.

## Storage / DAS
- **A few reduced-FS doc leftovers.** `fd2ba14` removed the main "Reduced variant" text, but
  `Docs/Services/Storage.md` still says "not in reduced file system" in the Create/Delete/Resize/
  Rename command rows, and `Docs/Devices.md:48` still reads "Memory: 128B (single file from offset
  0)" (the DAS is now a 512 B multi-file region at `0x3E00`). Optional wording cleanup.
- **`.SUBREQ` on a provider-only node** was reported once; the empty-table file is deleted now.
  Re-check if it reappears.

## Android (on-device behaviour untested)
- No Android device/emulator: BLE runtime permission prompt + denied/permanently-denied paths, BLE
  scan/connect/MTU, Storage Access Framework backup save + restore and file download, and the
  compact drawer shell on a phone form factor.

## Evaluation setup
- **LED brightness can brown out the board.** The builder clamps the displays to 5 % and the
  brightness script caps at 70 %, but a firmware-side current cap/ramp would be safer.
- **The LED display has no framebuffer readback**, so visuals are verified by eye only; a render
  snapshot command would make them testable.
- **DAS provider stale entries** - effectively solved by the 120 s provider lease + orphan-cancel
  path; revisit only if a *confirmed* cancel is wanted.
