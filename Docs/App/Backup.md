Unified tool for whole-network backup and restoration.
Works with the device's file systems (backup restoration), optional live restore. 
Creates a zipfile containing per device JSON files if backing up the entire network.

### Backup page
- Shows "Not connected" until a session is up.
- A checkbox list of the discovered devices (icon, name, ID and type); a stale device cannot
  be selected. Header shows "N of M selected" with a Select all / Deselect all toggle.
- "Include device files" switch (on by default) controls whether the device file systems go
  into the archive.
- **Create backup** writes the selected devices to a `.zip` chosen through the host file
  picker and reports the device/entry count.
- **Restore** picks a `.zip`, builds a restore plan and opens the plan page.

### Restore plan page
Per part selection of synced items, can sync to different device/block/part if target compatible to source.
- One expandable section per source device with a target-device dropdown (remap the whole
  device), then per block/script/subscription/SNDB/file entries as checkboxes.
- A block with more than one compatible target gets its own target dropdown.
- Items that cannot be placed are shown with an orange reason and cannot be ticked.
- Footer shows the selected / unavailable counts; **Restore** applies the checked items and
  reports written/failed counts.

The storage format is semantic (types, keys, enums, etc. are described in words), numbers are not used unless it's the literal value or index.
This makes the system more resilient against small firmware updates (reordering, adding/removing of fields).
