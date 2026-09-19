Lists all loaded/avaliable (stored) scripts on that device (switch in page name).

Each entry allows for showing the state of the script, and control of the script (start, pause/continue, stop, restart, etc...)
Expanding the entry shows input and outputs formatted with the UI specificiations (sliders, buttons, toggles, etc...).

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
Appbar contains button for checking function validity and uploading (file)/updating (live) to the device