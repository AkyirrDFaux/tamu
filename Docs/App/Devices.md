Main interaction layer.
## Main page
### Appbar
- (left) View selection
	- List view (scrollable)
	- Graph view (pannable)

- (right) Refresh button
### Bottom of screen
 - Filtering
	 - Net
	 - Device type
	 - Sorting (ID/Name/DeviceType)
### Main view (list variant)
- List all filtered devices in selected order
	- Use icons for rough device types
	- Display name
	- A smaller subtext displays the ID and Device type in text
	- Tap on entry opens [[Device view]]
### Main view (graph variant)
 - A graph view with the connection structure visible
	- Topmost cores (contents are given by SNDB)
	- Routers below (tree based on router tables)
	- Nodes (stacked vertically)
 - Each device is a block
	- Use icons for rough device types
	- Display name
	- A smaller subtext displays the ID and Device type in text
	- Tap on block opens [[Device view]]