Access to all avaliable system blocks
Automatically refreshes the visible view (0.5s by default).
### Appbar
- (right) Current/Backup view toggle
- Save button (current, "Save all") / Recall button (backup, "Recall all")
- Edit mode button (for dynamic blocks, only if they are supported by the device)
- Refresh button (with autorefresh function, opens a selection window, has a green dot if active)
- A busy spinner while a Save/Recall runs

### Main view (current)
- List containing nested lists
	- Main list entries show block name, block type, flags and field count
	- Sublists contain block's entries
		- Show type and flags and value of each entry
- Editable values can be edited when tapped (current)
- Show block and value entry names and units based on context  (the block types, enums and indexes).
- System struct fields expand into their keyed members; dynamic blocks use the flat
  (field, key) model, with dictionary (Geometry/Texture) fields showing named keys.
- A persistent field's menu offers Save to backup / Recall from backup.

### Backup view
- Shows the values a Save actually persisted, decoded from the device's `.SV` / `.DT_` / `.DV_`
  files, read-only, with a per-entry Recall button. A field with no stored entry is shown
  explicitly as not backed up.

### Edit view
- Allows for adding/editing/moving dynamic blocks
	- Add, rename, delete, and move a block to an index; drag to reorder
- Allows for adding/editing/moving dynamic block's entries
	- Add/delete/reorder fields, add/change/delete keys, change an entry's type
	- Includes changing passive flags
		- Read-only and Persistent flags per entry
