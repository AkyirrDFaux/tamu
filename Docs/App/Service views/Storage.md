A list of files stored on the device.
The file table is always read only (shown with a lock icon).
### Appbar
- Upload file (host OS → device; the device file name is the base name trimmed to 8 bytes)
- Create file (name max 8 chars + a byte size)
- Refresh (hold for autorefresh menu)

### Main view (list)
- One row per file: icon by known type, normalised file name, and a subtext with the file
  type label, size and storage offset.
- The read-only file-table file itself is listed separately (greyed, lock icon, "file table");
  tapping it opens the decoded directory (live records first, an expandable invalidated
  section after).
- Per-file actions (menu): View, Download, Rename, Delete. Tapping a file opens its viewer.

### File viewer
- The viewer/editor of files formats the files based on the file name to provide a human readable interpretation
  (serial registry `.SNREG`, LED layout `LAY*`, registry backups `.SV`/`.SUBREQ`, dynamic
  block table/values `.DT_`/`.DV_`, text `.TXT`/`.LOG`).
- Switching to hex reader is always possible (selectable bytes-per-line).
- Files are previewed, not edited in place.
