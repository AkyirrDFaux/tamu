Unified tool for whole-network backup and restoration.
Works with the device's file systems (backup restoration), optional live restore. 
Creates a zipfile containing per device JSON files if backing up the entire network.

Per part selection of synced items, can sync to different device/block/part if target compatible to source.

The storage format is semantic (types, keys, enums, etc. are described in words), numbers are not used unless it's the literal value or index.
This makes the system more resilient against small firmware updates (reordering, adding/removing of fields).