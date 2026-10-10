Basic device information and description. It shares memory internally with static memory, except for the precompiled and fixed information in fields 0 and 1.
### Block Type 0
`F.K:SP` is Field.Key:Struct Position.

| Name                        | F.K:SP | Flags | Size               | Note |
| --------------------------- | ------ | ----- | ------------------ | ---- |
| Device Type                 | 0.0:0  | RO    | uint32             |      |
| Capability                  | 0.0:1  | RO    | uint32             |      |
| Software version            | 0.0:2  | RO    | 32bit (7+4+5+16)   | YY:MM:DD:II, year-month-day-iteration |
| Serial Number               | 1      | RO    | Serial Number      |      |
| ID                          | 2      | RO    | uint16             |      |
| Uptime                      | 3.0:0  | RO    | uint32             | ms   |
| Current time                | 3.0:1  | RO    | uint32             | ms   |
| Time offset                 | 3.0:2  | RO    | int32              | ms   |
| Loop time                   | 3.0:3  | RO    | Number             | ms   |
| Max Loop time               | 3.0:4  | RO    | Number             | ms   |
| Used RAM                    | 4.0:0  | RO    | uint32             |      |
| Total RAM                   | 4.0:1  | RO    | uint32             |      |
| Used FLASH                  | 5.0:0  | RO    | uint32             | By the filesystem |
| Total FLASH                 | 5.0:1  | RO    | uint32             | Available to the filesystem |
| Name                        | 6      | P     | `Name`             | Applies fully after reboot |
| NetID                       | 7      | P     | uint8              | Core only; applies only after reboot |
| App Active                  | 8      | RO    | Enum               | Core only (No/USB/BLE/Legacy BT/WiFi) |
| WiFi connection information | 8.1    | P     |                    | SSID and Password. WiFi devices only, autoconnects |

If possible, the device name is shown in BLE advertising and on USB.

The Capability field is a 32bit field with one bit per entry:

	- Core
	- Router
	- Node
	- App Interface
	- Dynamic Memory
	- Scripts
	- Subscription Request
	- Subscription Provide
	- ...
### Commands for All Devices (000x)
| Name     | ID | Request        | Response                                             | Note |
| -------- | -- | -------------- | ---------------------------------------------------- | ---- |
| Discover | 0  | SN (from node) | SN (of node) + ID (from core, assignment)            | Sent from 0.0 |
| Ping     | 1  |                |                                                      |      |
| Identify | 2  | -              | -                                                    | Blink the red LED fast for 10 s |
| TimeSync | 3  | Local time     | Local time, foreign time received, foreign time sent | NTP-like |

Devices send the discover packet at random intervals until their ID is assigned, then TimeSync to the core. TimeSync repeats at a device-specific interval (see [[Devices]]) chosen to keep the total difference below 10 ms; a larger difference is occasionally acceptable. A slope and offset model tracks drift and offset.
### Core Functions
Use define `TYPE_CORE`.

The `NetID` is set by the user for each core. Zero is not allowed and is replaced by a randomly generated value.

After boot the core discover command is sent, and responses are expected within 500 ms.

`NetID` collisions are checked. If one occurs, the normal boot of that core is aborted; the core remains accessible through the app to change its `NetID`, the issue is logged, and the red LED blinks periodically.

If multiple cores are present, the core TimeSyncs to the longest running one, by uptime. After that it continues as normal, assigning IDs within the local net.

The core provides the time reference to its own net, and re-syncs its own time with the longest running core at a device-specific interval (see [[Devices]]), targeting under 10 ms total difference with the slope and offset model, jittered so the reference core is not periodically overwhelmed.
#### SNDB
Every core implements a persistent SN Database.

It has its own file, which stores Serial number and ID pairs.

Local devices are stored with NetID 0. Other cores are stored only with their NetID.1. Devices from foreign nets are not stored.

ID assignment first checks the database by SN; if nothing is found, a new ID is selected. The core's own `NetID` is always added automatically.
### Commands for Cores (001x)
| Name          | ID | Request                    | Response                        | Note |
| ------------- | -- | -------------------------- | ------------------------------- | ---- |
| Core discover | 0  | SN                         | SN, uptime                      | Core only; sent to 3F.1, from NetID.1 |
| SNDB Read     | 1  | ID or SN (based on length) | SN + ID                         |      |
| SNDB Write    | 2  | SN + ID                    | SN + ID                         | Setting the ID to 0 works as a delete. |
| SNDB Read All | 3  | -                          | Fragmentation, SN + ID (stream) |      |
