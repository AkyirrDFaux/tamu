The architecture the firmware follows: what it needs, the services it is built from, and the rules its code follows.
### Needs
- High and low capacity [[Devices]], varying in RAM, flash and CPU speed
- Uses [[RSBus and Packets]]
- Device-specific code for optimisation, covering architecture, GPIO, flash and similar
- A common command interface
- A red error LED and a white communication LED
- Everything aligned to 32 bits, though structs may be smaller inside
### Services
[[Command ID Table]] is the index of all command IDs.
#### Mandatory
- [[Services/System Block and Device Commands|System Block and Device Commands]], which implicitly covers Static Memory under [[Services/Register]]
- [[Services/Log Handler|Log Handler]]
- [[Services/Storage|Storage]]
#### Mandatory for Core
- SN Database, under [[Services/System Block and Device Commands|System Block and Device Commands]]
- [[Services/App Interface|App Interface]]
#### Mandatory for Routers
- [[Services/Router|Router]]
#### Mandatory for Sensor Nodes
- [[Services/Subscriptions|Subscriptions]], the provider side
#### Optional
- Dynamic memory, under [[Services/Register]]
- [[Services/Script|Script]]
### Data Formats
The value and payload types are defined in [[Data Formats]].
### Modules
Sets of predefined memory blocks and the functions that together provide one unified feature to the device. A module is included per device by its capabilities, and almost always has device-specific implementations.
