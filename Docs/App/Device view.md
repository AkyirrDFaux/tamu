A standalone page displaying known things about the device.
### Appbar
- (left) Device name (tap to rename; edits the device's **reported** name via the Register
  System Name field, max 16 bytes) with an edit icon
- (right) Identify button (Device service CID 2: blinks the device's red LED so you can find it), Refresh button (hold for autorefresh menu)

### First section - Device info
Identity fields come from the Register System block (type/SN/version/capability/name), the
same data the Device commands expose.
- ID, Net
- Device type
- Serial number
- Software version
- Uptime, average loop time, max loop time, time offset
- Capabilities (the text labels the device reports: Core, Router, DynMem, Scripts, Files,
  App, SubReq, SubProv, Node)
### Second section - Services
Per services avaliable, hide unavaliable (based on capabilities device field), list of links to specialized viewer
- [[App/Service views/Register|Register]]
- [[App/Service views/Storage|Storage]]
- [[App/Service views/Subscriptions|Subscriptions]] (only when the device has a subscription
  capability)
- [[App/Service views/Script|Scripts]] (only when the device has the Scripts capability)
- SN Database viewer (Core only)
- Log viewer (Core only - the log database lives on cores; nodes are outbound-only reporters)
- Router table viewer (Router only - not yet available; the firmware Router service is still a stub)
