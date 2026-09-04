### Needs:
- High and low capacity [[Devices]] (RAM, Flash, CPU speed)
- Uses [[RSBus]]
- Device specific code for optimalization (Architecture, GPIO, Flash, etc...)
- Common command interface
- Red error LED and white communication LED
- Align everything to 32-bit (structs may be smaller inside)

## Services:
[[Command ID table]]
#### Mandatory:
 - [[Services/System Memory|System Memory]] (implicitly [[Register]])
 - [[Log Handler]]
 - [[Storage]]
#### Mandatory for core:
- SN Database (under [[Services/System Memory|System Memory]])
- [[App Interface]]
- [[CLI]] (ESP32 only)
#### Mandatory for routers:
 - [[Router]]
#### Mandatory for all nodes:
- Static Memory (under [[Register]])
#### Mandatory for sensor nodes:
- [[Subscriptions]]
####  Optional:
 - Dynamic memory (under [[Register]])
 - [[Script]]
 
## [[Data Formats]]

## Modules
Sets of predefined memory blocks, and functions to provide an unified overall feature to the device. Included per device by it's capabilities, almost always has device specific implementations.
## Code rules:
Use PascalCase where possible
Each service or large feature should be a class.
Reuse code whereever if possible.
Use structs for data organization (command payloads, etc.).
Use malloc/realloc/free instead of new and delete.
Keep functions moderate in length (20ish lines).
Device specific implementations should have separate folders.
Do not import new libraries or use float/double type (not even standard ones, except for `<cstdint><cstddef><cstring><cstdlib>` and ones required within ESP32 scope).
Remember that this is an embedded system with limited resources.