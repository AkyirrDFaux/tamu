List the avaliable devices via the supported interface.
### Appbar buttons
- (left) Source selector (BLE devices / USB devices / All devices). USB entries are only
  offered where USB links exist (Linux desktop); Android offers BLE only.
- Refresh (hold for autorefresh menu, with a 1s period **on by default**, green dot while
  autorefresh is active, red if the last refresh errored)
- Sorting type (Signal strength / Alphabetical)

### Main window (list)
- A Bluetooth permission/availability banner is shown at the top when the radio is unusable
  or permission was denied (with an "Open settings" shortcut).
- While a session is being established a "Connecting to <target>" row is shown with a cancel
  button.
- Currently connected device
  - Reported device name (from the Register System block) as the title, link name as subtext
  - Autoconnect toggle (highlighted green when this device is the autoconnect target)
  - Disconnect button (right side)
- Remaining avaliable devices
  - Device name
  - Connection type (USB/BLE) icon
  - Signal strength, colorcoded (BLE only)
  - MAC (BLE only) or COM number / dev name (USB only)
  - On tap connect to that device (if another session is up it is closed first)
  - On long-press set that device as the autoconnect target
- Empty state reads "No devices found".
