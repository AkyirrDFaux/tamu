Basic device information and description, shares memory internally with static memory.
### Block type 0
F.K:SP = Field.Key:Struct Position

| Name             | F.K:SP | Flags | Size            | Note                                      |
| ---------------- | ------ | ----- | --------------- | ----------------------------------------- |
| Device Type      | 0.0:0  | RO    | 32bit           |                                           |
| Capability       | 0.0:1  | RO    | 32bit           |                                           |
| Software version | 0.0:2  | RO    | (uint8 x4)      | YY:MM:DD:II<br>(year-month-day-iteration) |
| Serial Number    | 1      | RO    | 14 bytes        |                                           |
| ID               | 2      | RO    | uint16          |                                           |
| Uptime           | 3.0:0  | RO    | uint32          | ms                                        |
| Current time     | 3.0:1  | RO    | uint32          | ms                                        |
| Time offset      | 3.0:2  | RO    | int32           | ms                                        |
| Loop time        | 3.0:3  | RO    | Number (32-bit) | ms                                        |
| Max Loop time    | 3.0:4  | RO    | Number (32-bit) | ms                                        |
| Used RAM         | 4.0:0  | RO    | uint32          |                                           |
| Total RAM        | 4.0:1  | RO    | uint32          |                                           |
| Used FLASH       | 5.0:0  | RO    | uint32          | By filesystem                             |
| Total FLASH      | 5.0:1  | RO    | uint32          | Avaliable to filesystem                   |
| Name             | 6      | P     | 16 bytes        | Applies fully after reboot                |
| NetID            | 7      | P     | uint8           | Core only, applies only after reboot      |
| App Active       | 8.0:0  | RO    | 1bit            | Core only                                 |
| CLI Active       | 8.0:1  | RO    | 1bit            | Core with CLI only                        |
If possible, the device name is shown in BLE advertising and on USB.

Capability (32bit-field):
	- Core
	- Router
	- Node
	- CLI
	- App Interface
	- Dynamic Memory
	- Scripts
	- Subscription Request
	- Subscription Provide
	- ...
### Commands for all devices (000x)

| Function | ID  | Content request | Content response                                     | Note                        |
| -------- | --- | --------------- | ---------------------------------------------------- | --------------------------- |
| Discover | 0   | SN (from node)  | SN (of node) + ID (from core, assignment)            | Sent from 0.0               |
| Ping     | 1   |                 |                                                      |                             |
| Identify | 2   | -               | -                                                    | Blink red LED fast for 10s. |
| TimeSync | 3   | Local time      | Local time, foreign time recieved, foreign time sent | NTP-like                    |
Devices (with random interval) send the discover packet until their ID is assigned.
Then they do a TimeSync to the core, repeated at random.
### Core functions
The net-ID is set by user for each core, 0 is not allowed, it's then randomly re-generated.
After boot the Core discover command is sent, responses are expected within 500 ms. 

Collisions of net-IDs are checked, if a collision happens, normal boot of that core is aborted. After abort it is still accessible via App or CLI to change the net-ID, the issue is logged, red LED blinks periodically.

If multiple cores are present, itself TimeSyncs to the longest running one (longest uptime).
After that the core continues as normal, assigning ID's within the local net.

Core provides time sync reference to it's own net. The core then randomly re-syncs it's own time synchronization with the longest running core.
#### SNDB
Every core implements persistent SN Database.
The SNDB has it's own file, which stores Serial number + ID pairs

Local devices are stored with NetID 0. Other cores are stored only with their NetID.1. Foreign net devices are not stored.

Assignment of IDs first checks the database using the SN, if nothing is found a new ID is selected.
The NetID of the core is always automatically added.
### Commands for cores (001x)

| Function      | ID  | Content request            | Content response                | Note                                  |
| ------------- | --- | -------------------------- | ------------------------------- | ------------------------------------- |
| Core discover | 10  | SN                         | SN, uptime                      | Core only, Sent to 3F.1, from NetID.1 |
| SNDB Read     | 11  | ID or SN (based on length) | SN + ID                         |                                       |
| SNDB Write    | 12  | SN + ID                    | SN + ID                         | Setting ID to 0 works as delete       |
| SNDB Read All | 13  | -                          | Fragmentation, SN + ID (stream) |                                       |