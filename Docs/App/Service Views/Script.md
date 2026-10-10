Lists all loaded/avaliable (stored) scripts on that device (Loaded/Available switch at the top of the page).

Each entry allows for showing the state of the script, and control of the script (start, pause/continue, stop, restart, etc...).
- A loaded entry shows a state dot, the function name, the file name, the state label and
  the instruction counter, and expands to its Input and Output sections.
- Controls: Start, Pause/Resume, Stop, Restart, Unload, and "Open editor".
- Input is interactive and editable (UI specifications read from the script file); Output
  shows live values read-only.
- The Available list shows stored `SCR_XXX` files with Open editor, Load (into the lowest
  free slot) and Delete; a "New script" button creates a new stored file (not loaded).

On tapping the entry's "Open editor" button opens the script editor page.
# Script editor
Shows everything in a organised human-readable format.
- Controls and state
- Inputs
	- Shows UI preview, interactive
	- Editing of the types, style, setting limits etc.
- Outputs
	- Editing of types and names
	- Shows live values
- Variables
	- Allows for adding/removing
	- Type specification
	- Shows live variable values
- Constants
	- Shows and allows for edits of constant values and names
- Instructions
	- Editing of the script on per-line and per-symbol basis
	- Everything is shown using names (Variables, I/O) or exact values (Predefines/no-name constants)
	- Currently active line is highlighted.
	- Symbol editor
		- Contains recommendations for that specific edit based on context (symbol position, instruction on that line)
		- Per symbol category split
		- Variable/Constant creation shortcuts.

Has to check type compability and program validity before sending to device.
The editor works on the stored `SCR_XXX` file, so loaded and available scripts alike are
editable. Appbar buttons: **Check validity**, **Upload** (write the file), and for a loaded
script **Update** (reload live to apply the draft) and **Unload**, plus a refresh button.
The file id and the loaded slot are independent; the app tracks the mapping for the loads it
starts (Issues.md, "Script CID 0 lists file IDs, not loaded slots").

# Script UI info
The stored script's UI-info blob is decoded only by the app (the firmware treats it as opaque
bytes; its length is the header's UI-info size field). Layout, version 2:

| Part           | Size                                                        | Note                                    |
| -------------- | ----------------------------------------------------------- | --------------------------------------- |
| Version        | uint8                                                       | UI-info format version; currently 2     |
| Function name  | length-prefixed                                             |                                         |
| Input names    | uint8 count, then length-prefixed names                     |                                         |
| Output names   | uint8 count, then length-prefixed names                     |                                         |
| Variable names | uint8 count, then length-prefixed names                     |                                         |
| Constant names | uint8 count, then length-prefixed names                     |                                         |
| Input UI       | per input: uint8 type, 3 reserved bytes, Number min, Number max, Number step |          |
| Input options  | per input: uint8 count, then length-prefixed labels         | Version 2; empty when the input has no choices |

A length-prefixed string is a `uint8` count followed by that many characters.
