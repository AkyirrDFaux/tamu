Extended idea from [[Dynamic Memory]], but it contains dictionaries with one more layer that are using keys.
One implementation for all devices.
Use define USE_KEYED_MEMORY

Fully user editable, dictionaries and keys are changable, appendable, removable.
Resides in ram and in backup storage file.
No callback functions.

First index to access block, second for block's dictionaries, third for dictionary entries/keys.
Separate value and metadata arrays for faster search.
All values have to be aligned to 32bits.
Metadata arrays are Dictionary BlockMeta (length is number of keys contained) followed by the dictionary's contained Key BlockMeta.

Name is treated as the value of the block itself, is part of definition.

Deletion is handled by setting the type, completely deallocated only if saved (to track the change).

[[Storage]] : Update atomically, flag old/invalid entries

|**BlockIndex**|**BlockMeta**|**Values**|
|---|---|---|
|4 bytes|4 bytes|N bytes (padded to 32bit)|

Service layout:

| **Function** | **SRV CID** | **Payload In**                   | **Payload out**                | **Note**                                                                                                                                             |
| ------------ | ----------- | -------------------------------- | ------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| Create       | 0           | BlockIndex + BlockMeta (+ Value) | BlockIndex + BlockMeta + Value | the part it's in must exist, respond only if requested                                                                                               |
| Delete       | 1           | BlockIndex                       | Status                         | Deletes value, sets type to deleted, respond only if requested                                                                                       |
| Read         | 2           | BlockIndex                       | BlockIndex + BlockMeta + Value | Invalid Block index reads number of blocks, Invalid Field number of fields, Invalid Key the dictionary BlockMeta, and value is keys (as uint8 array) |
| Write        | 3           | BlockIndex + BlockMeta(+ Value)  | BlockIndex + BlockMeta + Value | if it does not exist it gets created, writing type invalid deletes it, respond only if requested                                                     |
| Read backup  | 4           | BlockIndex                       | BlockIndex + BlockMeta + Value |                                                                                                                                                      |
| Save         | 5           | BlockIndex                       | Status                         | saves that entry (and everything inside), Invalid Block index saves everything, respond only if requested                                            |
| Recall       | 6           | BlockIndex                       | Status                         | recalls that entry (and everything inside), Invalid Block index recalls everything, respond only if requested                                        |