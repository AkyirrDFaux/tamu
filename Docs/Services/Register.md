Memory with 32-bit alignment. System, static and dynamic blocks covered.
Addressed via a 32-bit index (BlockInfo), split categorically by Blocks (and their instances), then Fields and Keys (3 levels).
Memory offsets are relative, in bytes, uint16 type.

| Struct    | Section       | Size       | Note           |
| --------- | ------------- | ---------- | -------------- |
| BlockInfo | BlockType     | 10bit/8bit | Static/Dynamic |
|           | BlockInstance | 6bit/8bit  | Static/Dynamic |
|           | Field         | uint8      |                |
|           | Key           | uint8      |                |
| ValueInfo | Type          | uint16     |                |
|           | Size          | uint8      | In bytes       |
|           | Flags         | 8bit       |                |

| Flag       | Description                                                                                                                |
| ---------- | -------------------------------------------------------------------------------------------------------------------------- |
| Read Only  | Non-writable from outside                                                                                                  |
| Persistent | This value is retained after reboot                                                                                        |
| Trigger    | Has a trigger on write, the function is called before the write itself, and applies the write instead of the standard one. |
#### Block types

| Block type         | Index       |
| ------------------ | ----------- |
| System             | 0           |
| Static block types | ...         |
| Dynamic            | 0x3F0-0x3F3 |
| Scripts            | 0x3F4-0x3F7 |
| Reserved           | 0x3F8-0x3FF |
### System + Static memory blocks
Basic flat memory, directly accesible internally by the device.
Compile time layout.
Write of a different type and/or length fails.

There are two memory sub-types, differentiated by the "Persistent" flag.
Separated for easier saving to flash to avoid long serialization and deserialization.

| Memory sub-type | Usage           | Internal access                               | External access             |
| --------------- | --------------- | --------------------------------------------- | --------------------------- |
| Volatile        | Everything else | Free                                          | Blockable by read only flag |
| Persistent      | Settings        | Read only except validity checking (triggers) | Free                        |
  The blocks are split into the two sub-type memory segments. Table routing goes from blocks, indexes and keys to the raw memory segments. 
#### Static Block Type Table

| Part                 | Section          | Size   | Note                                                                            |
| -------------------- | ---------------- | ------ | ------------------------------------------------------------------------------- |
| Count                | Entries          | uint16 |                                                                                 |
|                      | Triggers         | uint16 |                                                                                 |
| Array of entries (N) | Field&Key        | uint16 |                                                                                 |
|                      | MemoryOffset     | uint16 | Points to the value (for first instance) in volatile or persistent memory space |
|                      | ValueInfo        | 32bit  |                                                                                 |
| Trigger table (M)    | Field&Key        | uint16 |                                                                                 |
|                      | Function pointer | 32bit  |                                                                                 |

The order of stacking within memory subtypes is sorted by the BlockInfo (i.e. lowest BlockType, BlockInstance, Field and Key first), same as if sorted by the whole uint32.

Since blocks of same types have the same size usage within the memory sub-type, only one (first) offset of each variable has to be stored.
#### Triggers
If a write is happening on a field/key with the trigger flag, the trigger table is looked through for the right function. It is called before the write itself, and applies the write instead of the standard one.
#### Persistence
The storage is structurally 1:1 mirror of the memory in a file (.SV). The device can only overwrite/load the entire file. For targeted saves/recalls the app is needed.
### Basic commands (010x)

| Function         | ID  | Content request               | Content response                                                       | Note                                |
| ---------------- | --- | ----------------------------- | ---------------------------------------------------------------------- | ----------------------------------- |
| Enumerate blocks | 0   |                               | Fragmentation, Block types + maximum instance for each (uint16) stream |                                     |
| Enumerate fields | 1   | BlockType + instance (uint16) | Fragmentation, Field&Key (uint16) stream                               |                                     |
| Read             | 2   | BlockInfo                     | BlockInfo, ValueInfo, Value                                            | Single entry                        |
| Write            | 3   | BlockInfo, ValueInfo, Value   | Success                                                                | Respond when required, Single entry |
| Recall All       | 4   |                               | Success                                                                | Respond only if requested           |
| Save All         | 5   |                               | Success                                                                | Respond only if requested           |
Partial saving/recall is handled by app with direct file writes/direct register writes.
### Dynamic blocks
Use define USE_DYNAMIC_BLOCKS.
Value count, types and lengths can be changed.
Strict ascending order of the Field&Keys is maintained, same with the values themselves.
Any structural change must be done completely, including memory movement and updating the offsets.
Triggers not allowed.

#### Dynamic Block Table

| Part                 | Section      | Size     | Note                                                                            |
| -------------------- | ------------ | -------- | ------------------------------------------------------------------------------- |
| Name                 |              | 16 chars |                                                                                 |
| Count                | Entries      | uint16   |                                                                                 |
|                      | -            | 16 bit   | Padding/Reserved                                                                |
| Array of entries (N) | Field&Key    | uint16   |                                                                                 |
|                      | MemoryOffset | uint16   | Points to the value (for first instance) in volatile or persistent memory space |
|                      | ValueInfo    | 32bit    |                                                                                 |
Setting the type to None deletes the entry.
Indexes can be added arbitrarily as long they don't collide and it fits in memory.
Flags have to be intentionally set.
#### Dynamic Block Descriptor
Runtime only, each block maintains three separate memory spaces, one for table, one for each memory subtype for values.

| Part                | Section   | Size          | Note |
| ------------------- | --------- | ------------- | ---- |
| Dynamic Block Table |           | 32bit Pointer | Heap |
| Volatile Space      |           | 32bit Pointer | Heap |
| Persistent Space    |           | 32bit Pointer | Heap |
| Table Size          | Used      | uint16        |      |
|                     | Allocated | uint16        |      |
| Volatile Size       | Used      | uint16        |      |
|                     | Allocated | uint16        |      |
| Persistent Size     | Used      | uint16        |      |
|                     | Allocated | uint16        |      |
#### Persistence
The storage is structurally 1:1 mirror of the memory.
The whole block table is always saved into a separate file (.DT_XX).
The persistent values are also saved in their file (.DV_XX).
Changing the persistance flag moves the variable from one memory space to other.
### Dynamic commands (011x)
Basic Commands also work on dynamic blocks, these are extra.
Write (basic command) with type none works as delete here.
Create/write is in specified place, not an append neccesarily.

| Function       | ID  | Content request          | Content response | Note                      |
| -------------- | --- | ------------------------ | ---------------- | ------------------------- |
| Create Dynamic | 0   | Index (uint16)           | Success          | Respond only if requested |
| Delete Dynamic | 1   | Index (uint16)           | Success          | Respond only if requested |
| Get Name       | 2   | Index (uint16)           | 16 chars         |                           |
| Set Name       | 3   | Index (uint16), 16 chars | Success          | Respond only if requested |
