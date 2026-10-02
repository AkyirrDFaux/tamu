Memory segmented into 32-bit sections. System, static and dynamic blocks covered.
Addressed via a 32-bit register index, split categorically by Blocks (and their instances), then Fields and Keys (3 levels).
### Map entry

| Section       | Subsection    | Size   | Note                       |
| ------------- | ------------- | ------ | -------------------------- |
| BlockInfo     | BlockType     | 10bit  | Handled by switch          |
|               | BlockInstance | 6bit   | Handled by array indexing  |
|               | Field         | uint8  |                            |
|               | Key           | uint8  |                            |
| ValueInfo     | Type          | uint16 |                            |
|               | Size          | uint8  |                            |
|               | Flags         | 8bit   |                            |
| Memory offset |               | uint16 | Relative, 32-bit multiples |
Block tables contain Field, Key, ValueInfo and Memory offset.
Communication uses BlockInfo and ValueInfo.
#### ValueInfo Flags 
All are passive

| Flag       | Description                                                                                                                |
| ---------- | -------------------------------------------------------------------------------------------------------------------------- |
| Read Only  | Non-writable from outside                                                                                                  |
| Persistent | This value is retained after reboot                                                                                        |
| Trigger    | Has a trigger on write, the function is called before the write itself, and applies the write instead of the standard one. |
#### Block types

| Block type             | Index |
| ---------------------- | ----- |
| System                 | 0     |
| Static block types     | ...   |
| Scripts                | 0x3FE |
| Dynamic                | 0x3FF |
### System + Static memory blocks
Basic flat memory, directly accesible internally by the device.
Write of a different type and/or length fails.

There are two memory sub-types, differentiated by the "Persistent" flag.
Separated for easier saving to flash to avoid long serialization and deserialization.

| Memory sub-type | Usage           | Internal access                               | External access |
| --------------- | --------------- | --------------------------------------------- | --------------- |
| Volatile        | Everything else | Free                                          | Flag-dependent  |
| Persistent      | Settings        | Read only except validity checking (triggers) | Free            |
  The blocks are split into the two sub-type memory segments. Table routing goes from blocks, indexes and keys to the raw memory segments. 
#### Block Type Table

| Part                 | Section                                             | Size   | Note                                                                            |
| -------------------- | --------------------------------------------------- | ------ | ------------------------------------------------------------------------------- |
| Memory requirements  | Volatile size                                       | uint16 | in 32bit multiples, used size                                                   |
|                      | Persistent size                                     | uint16 | in 32bit multiples, used size                                                   |
| Count                | Entries                                             | uint16 |                                                                                 |
|                      | Triggers                                            | uint16 |                                                                                 |
| Array of entries (N) | Field&Key                                           | uint16 |                                                                                 |
|                      | ValueInfo                                           | 32bit  |                                                                                 |
|                      | MemoryOffset                                        | uint16 | Points to the value (for first instance) in volatile or persistent memory space |
| Trigger table (M)    | Field&Key                                           | uint16 |                                                                                 |
|                      | Function pointer (<br>Static) / Script ID (Dynamic) | 32bit  |                                                                                 |

The order of stacking within memory subtypes is sorted by the BlockInfo (i.e. lowest BlockType, BlockInstance, Field and Key first), same as if sorted by the whole uint32.

Since blocks of same types have the same size usage within the memory sub-type, only one (first) offset of each variable has to be stored.
#### Triggers
If a block entry has the trigger flag, after the finished write, the trigger table of that block type is searched through linearly for the relevant trigger, which is then run.
#### Persistence
The storage is structurally 1:1 mirror of the memory. The user selects what should be updated in the save.
A new file is created, the old and new data merged into it, and old file is deleted.
### Basic commands (010x)

| Function   | ID  | Content request               | Content response                                                       | Note                                |
| ---------- | --- | ----------------------------- | ---------------------------------------------------------------------- | ----------------------------------- |
| Enumerate  | 0   | None                          | Fragmentation, Block types + maximum instance for each (uint16 stream) |                                     |
|            |     | BlockType + instance (uint16) | Fragmentation, Field&Key (uint16) stream                               |                                     |
| Read       | 1   | BlockInfo                     | BlockInfo, ValueInfo, Value                                            | Single entry                        |
| Write      | 2   | BlockInfo, ValueInfo, Value   | Success                                                                | Respond when required, single entry |
| Recall All | 3   |                               | Success                                                                | Respond always                      |
| Save All   | 4   |                               | Success                                                                | Respond always                      |
Partial saving/recall is handled by app with direct file writes/direct register writes.
### Dynamic blocks
Use define USE_DYNAMIC_BLOCKS.
Value count, types and lengths can be changed.
Strict ascending order of the Field&Keys is maintained, same with the values themselves.
Any structural change must be done completely, including memory movement and updating the offsets.

Setting the type to None deletes the entry.
Indexes can be added arbitrarily as long they don't collide and it fits in memory.
Flags have to be intentionally set.
#### Dynamic Block Descriptor
Runtime only, each block maintains three separate memory spaces, one for table, one for each memory subtype for values.

| Part               | Section   | Size          | Note               |
| ------------------ | --------- | ------------- | ------------------ |
| Name               |           | 12 chars      |                    |
| Block (Type) Table |           | 32bit Pointer | Heap               |
| Volatile Space     |           | 32bit Pointer | Heap               |
| Persistent Space   |           | 32bit Pointer | Heap               |
| Table Size         | Used      | uint16        | in 32bit multiples |
|                    | Allocated | uint16        | in 32bit multiples |
| Volatile Size      | Allocated | uint16        | in 32bit multiples |
| Persistent Size    | Allocated | uint16        | in 32bit multiples |

#### Persistence
The storage is structurally 1:1 mirror of the memory.
The whole block table is always saved into a separate file (DT_XXX).
The persistent values are also saved in their file (DV_XXX).
Changing the persistance flag moves the variable from one memory space to other.

### Dynamic commands (011x)
Basic Commands also work on dynamic blocks, these are extra.
Write (basic command) with type none works as delete here.
Create/write is in specified place, not an append neccesarily.

| Function         | ID  | Content request | Content response | Note                           |
| ---------------- | --- | --------------- | ---------------- | ------------------------------ |
| Create Dynamic   | 0   | Index           | Success          |                                |
| Delete Dynamic   | 1   | Index           | Success          |                                |
| Get Name         | 2   | Index           | 12 chars         |                                |
| Set Name         | 3   | Index, 12 chars | Success          | Respond only if requested      |
| Get Memory Usage | 4   | Index           | uint16x6         | Direct from descriptor + table |
