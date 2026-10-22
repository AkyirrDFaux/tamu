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

A restore cancels the requester entries the archive does not carry, so the device's requester table
ends up equal to the archive's. The cancellation is part of the restore and takes effect at once, not
at the next boot. The provider table is never part of an archive — it is not persistent — so a restore
never writes provider rows; a device re-forms them when its requesters re-request.

The storage format is semantic (types, keys, enums, etc. are described in words), numbers are not used unless it's the literal value or index.
This makes the system more resilient against small firmware updates (reordering, adding/removing of fields).

The archive functionally mirrors the persistence-giving state of each device; it is version
independent only because that state is described semantically. A per-device JSON carries the
Register's persistent entries, every dynamic block's whole table (a non-persistent row travels as
structure, without a value), the requester subscription table, the Scripts, the LED layouts, the
SNDB of a core and the device's files. State that no persistence file holds is absent: a static
block's non-persistent field is not archived, and neither is the provider subscription table, which
lives in RAM.

A file is archived as a binary only when it has no semantic form: a `SCR_XXX` script is stored
only as the semantic script and the file is rebuilt on restore, so a file that has a semantic
form never also travels as raw bytes.
