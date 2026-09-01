This service can update ID, report device type, compiled software version, serial number, capability, uptime, and provides time synchronization to core.
One implementation for all devices, extended by SNDB for cores (also single implementation).

If device has core capability, it can assign IDs, and store them in the registry.
If the serial number is already stored, it assigns the same ID as before.

Else, it sends discoverery packets if no ID (0) was assigned to the device periodically and waits for a message with it's serial number.

Specific packet function is defined as an enum in the service custom identifier portion.

### Service CIDs

| Function               | SRV CID | Content request                      | Content response                                               | Note                                                                                               |
| ---------------------- | ------- | ------------------------------------ | -------------------------------------------------------------- | -------------------------------------------------------------------------------------------------- |
| Discover (node - core) | 0       | SN (from node)                       | SN (of node) + ID (from core)                                  | Sent from 0.0                                                                                      |
| Discover (core - core) | 0       | SN                                   | SN                                                             | Sent to FFF.1, from NetID.1, request triggers response                                             |
| Ping                   | 1       | uint32                               | uint32 (same)                                                  | Random transaction ID                                                                              |
| Identify               | 2       | Bool                                 | -                                                              | True = blink red led fast, false = leave led alone                                                 |
| Silent mode            | 3       | Bool (true = silent, false = normal) | -                                                              | Will silence the device on the bus until requested otherwise, used for running bootloader in place |
| Device type            | 4       | -                                    | Device type enum                                               |                                                                                                    |
| Serial number          | 5       | -                                    | SN                                                             |                                                                                                    |
| Software version       | 6       | -                                    | Software version (uint8 x4)                                    | YY:MM:DD:II (year-month-day-iteration)                                                             |
| Capability             | 7       | -                                    | Capability bitfield (32 bit)                                   |                                                                                                    |
| Read Name              | 8       | -                                    | Name                                                           |                                                                                                    |
| Set Name               | 9       | Name                                 | Name                                                           | Respond only if requested                                                                          |
| Uptime                 | 10      | -                                    | Uptime (uint32)                                                | ms                                                                                                 |
| Loop Time              | 11      | -                                    | Average and Maximum loop time (2x Number)                      | ms                                                                                                 |
| Time sync              | 12      | Time sent                            | Original time sent, Local time recieved, Local time reply sent | ms                                                                                                 |
| Get time offset        | 13      | -                                    | Time offset (int32)                                            | ms                                                                                                 |
| Set time offset        | 14      | Time offset (int32)                  | -                                                              | ms                                                                                                 |
| Get NetID              | 15      | -                                    | NetID                                                          | Core only                                                                                          |
| Set NetID              | 16      | NetID                                | NetID                                                          | Core only, will take effect after reboot                                                           |
| SNDB Read All          | 17      | -                                    | Fragmentation, SN + ID (stream)                                | Core only                                                                                          |
| SNDB Read              | 18      | ID or SN (based on length)           | SN + ID                                                        | Core only                                                                                          |
| SNDB Write             | 19      | SN + ID                              | SN + ID                                                        | Core only, setting ID to 0 works as delete                                                         |

Device name is stored in standalone file to allow persistence.
If possible, the device name is shown in BLE advertising and on USB.

Every core implements SN Database.
The SNDB has it's own file, which stores (persistent) Serial number + ID pairs

Core also provides time sync reference. After recieving ID assignment, the newly discovered device sends a single initial timesync packet to the core to sync it's own time.

The core then randomly syncs other devices if idle to maintain synchronization.

#### Multi-core connection:
The net-ID is set by user for each core. Collisions of net-IDs are checked at start using discover packet, at each core's boot (500 ms timeout).
If they are valid, they sync their time to the earliest booted one.

No nodes allowed on crossing nets (discovery issues).
Manually specify branch bus is a net-crossing, otherwise assume it's local net.