Mostly I/O blocks, simple settings.
Each module has one memory block.
One implementation for all devices.

Type, structure of block, value types and lengths are fully defined and compiled in.
Has no name.
Option for a callback function on change.
Value and metadata flags in ram and values also in backup in storage file.

First index to access block, second for block's entries (Use BlockIndex struct).

[[Storage]] : Update atomically, flag old/invalid entries

| BlockIndex | BlockMeta | Values                    |
| ---------- | --------- | ------------------------- |
| 4 bytes    | 4 bytes   | N bytes (padded to 32bit) |

Read only entries are never saved, script updated entries only upon manual request

| Function    | SRV CID | Payload In                  | Payload out                 | Note                                                                                                          |
| ----------- | ------- | --------------------------- | --------------------------- | ------------------------------------------------------------------------------------------------------------- |
| Read        | 2       | BlockIndex                  | BlockIndex+BlockMeta+Value  | Invalid Block index reads number of blocks, Invalid Field number of fields                                    |
| Write       | 3       | BlockIndex +BlockMeta+Value | BlockIndex +BlockMeta+Value | respond only if requested                                                                                     |
| Read backup | 4       | BlockIndex                  | BlockIndex+BlockMeta+Value  |                                                                                                               |
| Save        | 5       | BlockIndex                  | Status                      | saves that entry (and everything inside), Invalid Block index saves everything, respond only if requested     |
| Recall      | 6       | BlockIndex                  | Status                      | recalls that entry (and everything inside), Invalid Block index recalls everything, respond only if requested |

#### Get estimate:
Split into more CID (add read name, get number of entries, split saving and recall into individual and full)