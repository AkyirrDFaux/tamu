Memory segmented into 32-bit sections. System, static and dynamic blocks covered.
Addressed via a 32-bit register index, split categorically by Blocks, then Fields and Keys (3 levels).
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

| Flag            | Type    | Description                                                                                     |
| --------------- | ------- | ----------------------------------------------------------------------------------------------- |
| ReadOnly        | Passive | Non-writable from outside                                                                       |
| Persistent      | Passive | This value is retained after reboot                                                             |
| Trigger         | Passive | Has a trigger on write                                                                          |
| -               | -       |                                                                                                 |
| NotSaved        | Active  | A change was been made compared to the saved state (with persistent only)                       |
| ScriptUpdated   | Active  | A script changed this variable                                                                  |
| External origin | Active  | A subscription is updating this variable or a different device updated this via script (paired) |
| -               | -       | -                                                                                               |
#### Block types

| Block type         | Index |
| ------------------ | ----- |
| System             | 0     |
| Static block types | ...   |
| Scripts            | 0x2FF |
| Dynamic            | 0x3FF |
### System + Static memory blocks
Basic flat memory, directly accesible internally by the device.
Write of a different type and/or length fails.
A read combines the active and passive flags together.

There are two memory sub-types, differentiated by the "Persistent" flag.
Separated for easier saving to flash to avoid serialization and deserialization.

They both have a 4-bit active flag field associated with each 32-bit section (retrievable via the offset or directly pre-compiled for internal use).

| Memory sub-type | Usage           |
| --------------- | --------------- |
| Volatile        | Everything else |
| Persistent      | Settings        |
  The blocks are split into the two sub-type memory segments. Table routing goes from blocks, indexes and keys to the raw memory segments. 
#### Block Type Table

| Part                 | Section          | Size   | Note                                                                            |
| -------------------- | ---------------- | ------ | ------------------------------------------------------------------------------- |
| Array of entries (N) | Field&Key        | uint16 |                                                                                 |
|                      | ValueInfo        | 32bit  | Only passive flags here                                                         |
|                      | MemoryOffset     | uint16 | Points to the value (for first instance) in volatile or persistent memory space |
| Trigger table (M)    | Field&Key        | uint16 |                                                                                 |
|                      | Function pointer | 32bit  |                                                                                 |
Number of entries and triggers and space requirements for each memory subtype is to be precompiled.

The order of stacking within memory subtypes is sorted by the BlockInfo (i.e. lowest BlockType, BlockInstance, Field and Key first), same as if sorted by the whole uint32.

Since blocks of same types have the same size usage within the memory sub-type, only one (first) offset of each variable has to be stored.
#### Triggers
If a block entry has the trigger flag, after the finished write, the trigger table of that block type is searched through linearly for the relevant trigger, which is then run.
#### Persistence
The storage is structurally 1:1 mirror of the memory. The user selects what should be updated in the save.
A new file is created, the old and new data merged into it, and old file is deleted.
Active flags don't get saved.
### Basic commands (01.0x)

| Function  | ID  | Content request                     | Content response                           | Note                                                     |
| --------- | --- | ----------------------------------- | ------------------------------------------ | -------------------------------------------------------- |
| Enumerate | 0   | -                                   | All avaliable block types                  |                                                          |
|           |     | Enum (0), BlockInfo                 | All avaliable instances of that block type |                                                          |
|           |     | Enum (1), BlockInfo                 | All avaliable fields in that block         |                                                          |
|           |     | Enum (2), BlockInfo                 | All avaliable keys in that field           |                                                          |
| Read      | 1   | BlockInfo (N)                       | BlockInfo, ValueInfo, Value (all-N)        | If it does not fit, send another packet, do not fragment |
| Write     | 2   | BlockInfo, ValueInfo, Value (all-N) | Success                                    | Respond only if requested, do not fragment input         |
| Save      | 3   | BlockInfo (N)                       | Success                                    | Respond only if requested                                |
| Recall    | 4   | BlockInfo (N)                       | Success                                    | Respond only if requested                                |

### Dynamic blocks
Value count, types and lengths can be changed.
Strict ascending order of the Field&Keys is maintained, same with the values themselves.
Any structural change must be done completely, including memory movement and updating the offsets.
Setting the type to None deletes the entry.
Indexes can be added arbitrarily as long they don't collide and it fits in memory.

Passive flags have to be intentionally set with separate command.
#### Dynamic Block Table

| Part                 | Section      | Size     | Note                                                                            |
| -------------------- | ------------ | -------- | ------------------------------------------------------------------------------- |
| Name                 |              | 12 chars |                                                                                 |
| Count                | Entries      | uint16   |                                                                                 |
|                      | Triggers     | uint16   |                                                                                 |
| Array of entries (N) | Field&Key    | uint16   |                                                                                 |
|                      | ValueInfo    | 32bit    | All flags here                                                                  |
|                      | MemoryOffset | uint16   | Points to the value (for first instance) in volatile or persistent memory space |
| Trigger table (M)    | Field&Key    | uint16   |                                                                                 |
|                      | Script ID    | 32bit    |                                                                                 |

#### Dynamic Block Descriptor
Runtime only, each block maintains three separate memory spaces, one for table, one for each memory subtype for values.

| Part                | Section   | Size          | Note        |
| ------------------- | --------- | ------------- | ----------- |
| Dynamic Block Table |           | 32bit Pointer | Heap/Static |
| Volatile Space      |           | 32bit Pointer | Heap/Static |
| Persistent Space    |           | 32bit Pointer | Heap/Static |
| Table Size          | Used      | uint32        |             |
|                     | Allocated | uint32        |             |
| Volatile Size       | Used      | uint32        |             |
|                     | Allocated | uint32        |             |
| Persistent Size     | Used      | uint32        |             |
|                     | Allocated | uint32        |             |
#### Persistence
The storage is structurally 1:1 mirror of the memory.
The whole block table is always saved into a separate file (DT_XXX).
The persistent values are also saved in their file (DV_XXX).
Changing the persistance flag moves the variable from one memory space to other.

### Dynamic commands (01.1x)
Basic Commands also work on dynamic blocks, these are extra.

| Function         | ID  | Content request     | Content response | Note                      |
| ---------------- | --- | ------------------- | ---------------- | ------------------------- |
| Create Dynamic   | 0   | BlockInfo           | Success          |                           |
| Delete Dynamic   | 1   | BlockInfo           | Success          |                           |
| Get Name         | 2   | BlockInfo           | 12 chars         |                           |
| Set Name         | 3   | BlockInfo, 12 chars | Success          | Respond only if requested |
| Get Memory Usage | 4   | BlockInfo           | uint32x6         | Direct from descriptor    |
