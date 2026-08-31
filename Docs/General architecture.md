### Needs:
- High and low capacity [[Devices]] (RAM, Flash, CPU speed)
- Uses [[RSBus]]
- Device specific code for optimalization (Architecture, GPIO, Flash, etc...)
- Common data interface with services as senders or endpoints
- Red error LED and white communication LED

## Services:
[[Service ID table]]
#### Mandatory:
 - [[Device service]]
 - [[Log Handler]]
 - [[Storage]]
 - [[System Memory]]
#### Mandatory for core (define TYPE_CORE):
- SN Database (under [[Device service]])
- App service (route to USB/BLE, TODO later)
- [[CLI]] (ESP32 only)
#### Mandatory for devices without USB:
- [[Bootloader]]
####  Optional:
 - [[Dynamic Memory]]
 - [[Keyed Memory]]
 - [[Router]]
 - [[Script]]
 
## [[Data Formats]]

## Modules
Sets of predefined memory blocks, and functions to provide an unified overall feature to the device. Included per device by it's capabilities, almost always has device specific implementations at least partially.

Unknowns:
	Subscriptions?
	Core synchronisation?

### Code rules:
Use PascalCase where possible
Each service or large feature should be a class.
Reuse functions if possible.
Use structs for data organization (packets).
Use malloc/realloc/free instead of new and delete.
Keep functions moderate in length (20ish lines).
Device specific implementations should have separate folders.
Do not import new libraries or use float/double type (not even standard ones, except for `<cstdint><cstddef><cstring><cstdlib>` and ones required within ESP32 scope).
Remember that this is an embedded system with limited resources

Code structure (inside src)
Main.cpp
Devices (includes device specific implementations)
| Tamu
|| Main.h
|| ...
| ...
Core (Common features)
| Services (Generic service interaction via SRV CID)
| Functions (Main and utility function definition and implementation if not device specific)
| Types (Number, Colour, Vector, Enums, ...)
| ...
Blocks (Reusable blocks and algorithms)
| PWM
| AccGyr
| ...
...