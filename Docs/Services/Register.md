Memory with 32-bit alignment, covering system, static and dynamic blocks. It is addressed through a 32-bit index, `BlockInfo`, split categorically into blocks and their instances, then fields and keys, three levels in total. Memory offsets are relative and in bytes, of type `uint16`.

| Section   | Part          | Size  | Note     |
| --------- | ------------- | ----- | -------- |
| BlockInfo | BlockType     | 10bit |          |
|           | BlockInstance | 6bit  |          |
|           | Field         | uint8 |          |
|           | Key           | uint8 |          |
| ValueInfo | Type          | uint16|          |
|           | Size          | uint8 | In bytes |
|           | Flags         | 8bit  |          |

| Flag       | Description |
| ---------- | ----------- |
| Read Only  | Non-writable from outside |
| Persistent | Retained after reboot |
| Trigger    | A write to this field calls a function before the write itself, and that function applies the write instead of the standard one. |
#### Block Types
| Block Type         | Index       | Note            |
| ------------------ | ----------- | --------------- |
| System             | 0           |                 |
| Static block types | ...         |                 |
| Dynamic            | 0x3F0-0x3F3 | Instances 0-63 per type; global index 0-255 across the four banks |
| Scripts            | 0x3F4-0x3F7 | Instances 0-63 per type; global index 0-255 across the four banks |
| Reserved           | 0x3F8-0x3FF |                 |
### System + Static Memory Blocks
Basic flat memory, directly accessible internally by the device, with a compile-time layout. A write of a different type or length fails; `String` and `Filename` writes may be shorter and are space-padded to the field size.

Two memory sub-types are distinguished by the `Persistent` flag, and are kept separate so that saving to flash avoids long serialisation and deserialisation.

| Memory Sub-type | Usage           | Internal Access                               | External Access             |
| --------------- | --------------- | --------------------------------------------- | --------------------------- |
| Volatile        | Everything else | Free                                          | Blockable by read only flag |
| Persistent      | Settings        | Read only except validity checking (triggers) | Free                        |

The blocks are split into the two sub-type memory segments. Table routing goes from blocks, indexes and keys to the raw memory segments.
#### Static Block Type Table
| Section              | Part             | Size   | Note                                                                            |
| -------------------- | ---------------- | ------ | ------------------------------------------------------------------------------- |
| Count                | Entries          | uint16 |                                                                                 |
|                      | Triggers         | uint16 |                                                                                 |
| Array of entries (N) | Field&Key        | uint16 |                                                                                 |
|                      | MemoryOffset     | uint16 | Points to the value (for first instance) in volatile or persistent memory space |
|                      | ValueInfo        | uint32 |                                                                                 |
| Trigger table (M)    | Field&Key        | uint16 |                                                                                 |
|                      | Function pointer | uint32 |                                                                                 |

The order of stacking within the memory sub-types is sorted by `BlockInfo`, that is by the lowest BlockType, BlockInstance, Field and Key first, the same as sorting by the whole `uint32`.

Since blocks of the same type use the same amount of space within a memory sub-type, only one offset, the first, has to be stored per variable.
#### Triggers
When a write happens on a field or key with the trigger flag, the trigger table is searched for the matching function. It is called before the write itself and applies the write instead of the standard one.
#### Persistence
Storage is a structural 1:1 mirror of the persistent memory, held in a `.SV` file. The device can only overwrite or load the whole file; targeted saves and recalls are handled by [[App/Service Views/Register|the app]].
### Basic Commands (010x)
| Name             | ID | Request                                             | Response                                                               | Note                                |
| ---------------- | -- | --------------------------------------------------- | ---------------------------------------------------------------------- | ----------------------------------- |
| Enumerate blocks | 0  |                                                     | Fragmentation, one word per present type: 10.6 (type, max instance) for the System and static types, 8.8 (bank type, highest global index) for the dynamic range |                                     |
| Enumerate fields | 1  | (BlockType << 6) + instance (uint16, 4-byte padded) | Fragmentation, Field&Key (uint16) stream                               | Return empty if nonexistent         |
| Read             | 2  | `BlockInfo`                                         | `BlockInfo`, `ValueInfo`, Value                                            | Single entry                        |
| Write            | 3  | `BlockInfo`, `ValueInfo`, Value                         | Success (echoes the request)                                           | Respond when required, single entry |
| Recall All       | 4  |                                                     | Success (bool)                                                         | Respond only if requested           |
| Save All         | 5  |                                                     | Success (bool)                                                         | Respond only if requested           |

A read with field `0xFF`, any key, returns a block's metadata: `BlockInfo`, `ValueInfo` with the field count in Size, then the 16-character name.

Partial saving and recall are handled by the app, through direct file writes and register writes.
### Dynamic Blocks
Use define `USE_DYNAMIC_BLOCKS`.

The value count, types and lengths can be changed. A strict ascending order of the Field&Keys is maintained, and the values follow the same order. Any structural change must be complete, including the memory movement and the offset updates. Triggers are not allowed.
#### Dynamic Block Table
| Section              | Part         | Size     | Note                                                                            |
| -------------------- | ------------ | -------- | ------------------------------------------------------------------------------- |
| Name                 |              | `Name` |                                                                                 |
| Count                | Entries      | uint16   |                                                                                 |
|                      | -            | 16bit    | Padding/Reserved                                                                |
| Array of entries (N) | Field&Key    | uint16   |                                                                                 |
|                      | MemoryOffset | uint16   | Points to the value (for first instance) in volatile or persistent memory space |
|                      | ValueInfo    | uint32   |                                                                                 |

Setting the type to None deletes the entry. Indexes can be added arbitrarily, as long as they do not collide and the block fits in memory. Flags have to be set intentionally.
#### Dynamic Block Descriptor
Runtime only. Each block maintains three separate memory spaces: one for the table, and one for each memory sub-type of values.

| Section             | Part      | Size   | Note                    |
| ------------------- | --------- | ------ | ----------------------- |
| Dynamic Block Table |           | uint32 | Pointer into the heap   |
| Volatile Space      |           | uint32 | Pointer into the heap   |
| Persistent Space    |           | uint32 | Pointer into the heap   |
| Table Size          | Used      | uint16 |                         |
|                     | Allocated | uint16 |                         |
| Volatile Size       | Used      | uint16 |                         |
|                     | Allocated | uint16 |                         |
| Persistent Size     | Used      | uint16 |                         |
|                     | Allocated | uint16 |                         |
#### Persistence
Storage is a structural 1:1 mirror of the persistent memory. The whole block table is always saved into its own file, `.DT_XX`, and the persistent values are saved in theirs, `.DV_XX`. Changing the persistence flag moves the variable from one memory space to the other.
### Dynamic Commands (011x)
The basic commands also work on dynamic blocks; these are extra. A basic Write with type None works as a delete here. Create and write place data at a specified location, not necessarily as an append.

| Name           | ID | Request                  | Response      | Note                      |
| -------------- | -- | ------------------------ | ------------- | ------------------------- |
| Create Dynamic | 0  | Index (uint16), `Name`   | Success (bool)| Respond only if requested |
| Delete Dynamic | 1  | Index (uint16)           | Success (bool)| Respond only if requested |
| Get Name       | 2  | Index (uint16)           | `Name`        |                           |
| Set Name       | 3  | Index (uint16), `Name` | Success (bool)| Respond only if requested |
