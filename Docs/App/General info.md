Written in flutter.
#### Target platforms:

| OS      | Connection types | Status               |
| ------- | ---------------- | -------------------- |
| Android | BLE              | Do not implement yet |
| Linux   | BLE, USB         | In development       |
| Windows | BLE, USB         | Do not implement yet |
### Overall app layout:
- [[Connection]]
- [[App/Devices|Devices]]
- [[Backup]]
- [[Settings]]
### Overall function
Interface the services in the different device's firmware via packets, provide a user friendly way to display information and control the devices.
Can do multiple things at once due to having a full CID range avaliable used as transaction ID.
Always can access the device it's connected to directly via ID 1 (Net 0), where it can obtain information about the rest of the system.
App keeps local database in RAM, updates upon arrival of new data.

### Rules
Avoid unnecessary libraries, remove if unused
Keep the theme (Grey-ish blue, orange, white)
Use recommended Flutter/Dart syntax