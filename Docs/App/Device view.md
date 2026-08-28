A standalone page displaying known things about the device.
### Appbar
- (left) Device name (rename option)
- (right) Identify button (Device service CID 2: blinks the device's red LED so you can find it), Refresh button

### First section - Device info
Interacts with the device service only
- Device type, capabilities (bitfield - Core, Router, CLI, Dynamic/Keyed Memory, Scripts, Bootloader)
- Serial number
- Software version
- Uptime, looptimes, time offset
### Second section - Services
Per services avaliable, hide unavaliable (based on capabilities device field), list of links to specialized viewer
- [[App/Service views/System Memory|System Memory]]
- [[App/Service views/Dynamic Memory|Dynamic Memory]]
- [[App/Service views/Keyed Memory|Keyed Memory]]
- [[App/Service views/Storage|Storage]]
- SNDB viewer (Core only)
- Router table viewer (Router only)
- Log viewer
- [[App/Service views/Script|Script]]