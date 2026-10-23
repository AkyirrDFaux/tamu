Main interaction layer.
### Main Page
#### Appbar
- (left) View selection
	- List view (scrollable)
	- Graph view (pannable)

- (right) Refresh button (hold for autorefresh menu)
#### Bottom of Screen
 - Filtering (tapping a chip cycles through its values)
	 - Net
	 - Device type
	 - Sorting (ID/Name/DeviceType)
#### Main View (List Variant)
- List all filtered devices in selected order
	- Use icons for rough device types
	- Display name (a stale entry is marked " · stale", dimmed)
	- A smaller subtext displays the ID and Device type in text
	- Cores carry a star marker
	- Tap on entry opens [[Device View]]
#### Main View (Graph Variant)
 - A pannable/zoomable graph view
	- Cores drawn topmost (their contents are given by SNDB)
	- Nodes stacked below in rows of three
 - Each device is a block
	- Use icons for rough device types
	- Display name
	- A smaller subtext displays the ID and Device type in text
	- Tap on block opens [[Device View]]
 - The router tree / net hierarchy is not drawn yet (routers are not implemented in firmware);
   blocks are grouped cores-first, then nodes.
#### States
- Not connected shows "Not connected".
- Nothing discovered yet shows the last error or "No devices discovered yet".
