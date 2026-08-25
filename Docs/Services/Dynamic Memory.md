Same idea as [[System Memory]], but user editable.
One implementation for all devices.
Use define USE_DYNAMIC_MEMORY

Type, name (16char), structure of block, values, their types and metadata flags are user editable.
Name is treated as the value of the block itself, is part of definition.
Everything resides in ram and in backup storage file.
No callback functions.

First index to access block, second for block's entries.
Separate BlockMeta arrays for faster search.
All values have to be aligned to 32bits.
Deletion is handled by setting the type, completely deallocated only if saved (to track the change). Indexes do not change with block or value deletion (no move).

[[Storage]] : Update atomically, flag old/invalid entries

| BlockIndex | BlockMeta | Values                    |
| ---------- | --------- | ------------------------- |
| 4 bytes    | 4 bytes   | N bytes (padded to 32bit) |

Service layout:

| Function    | SRV CID | Payload In                       | Payload out                    | Note                                                                                                          |
| ----------- | ------- | -------------------------------- | ------------------------------ | ------------------------------------------------------------------------------------------------------------- |
| Create      | 0       | BlockIndex + BlockMeta (+ Value) | BlockIndex + BlockMeta + Value | the part it's in must exist, respond only if requested                                                        |
| Delete      | 1       | BlockIndex                       | Status                         | Deletes value, sets type to none, respond only if requested                                                   |
| Read        | 2       | BlockIndex                       | BlockIndex + BlockMeta + Value | Invalid Block index reads number of blocks, Invalid Field number of fields, and name of block                 |
| Write       | 3       | BlockIndex + BlockMeta(+ Value)  | BlockIndex + BlockMeta + Value | if it does not exist it gets created, index padding is by none type, respond only if requested                |
| Read backup | 4       | BlockIndex                       | BlockIndex + BlockMeta + Value |                                                                                                               |
| Save        | 5       | BlockIndex                       | Status                         | saves that entry (and everything inside), Invalid Block index saves everything, respond only if requested     |
| Recall      | 6       | BlockIndex                       | Status                         | recalls that entry (and everything inside), Invalid Block index recalls everything, respond only if requested |

