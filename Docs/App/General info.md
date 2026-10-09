Written in flutter.
#### Target platforms:

| OS      | Connection types |
| ------- | ---------------- |
| Android | BLE              |
| Linux   | BLE, USB         |
| Windows | BLE, USB         |
### Overall app layout:
The app shell holds five tabs, in this order (phone/tablet shows them in a drawer, a wide
window keeps a permanent navigation rail). The tab bar also carries a small link badge
(`LINK`/`OFF`) showing whether a device session is up.
- [[Connection]]
- [[App/Devices|Devices]]
- [[Update]]
- [[Backup]]
- [[Settings]]
### Overall function
Interface the different device's firmware via commands, provide a user friendly way to display information and control the devices.
A large focus on human-readable interpretation, targeted at technically non-proficient users, while subtly displaying extra information for more advanced users.
Can do multiple things at once due to having a reserved TRID range.
Always can access the core device it's connected to via ID 0.1, where it can obtain information about the rest of the system.
App keeps local database in RAM, updates upon arrival of new data.

### Rules
Avoid unnecessary libraries, remove if unused
Keep the theme (Grey-ish blue, orange, white)
Use recommended Flutter/Dart syntax
