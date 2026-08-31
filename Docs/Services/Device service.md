This service can update ID, report device type, compiled software version, serial number, capability, uptime, and provides time synchronization to core.
One implementation for all devices, extended by SNDB for cores (also single implementation).

If device has core capability, it can assign IDs, and store them in the registry.
If the serial number is already stored, it assigns the same ID as before.

Else, it sends discoverery packets if no ID (0) was assigned to the device periodically and waits for a message with it's serial number.

Specific packet function is defined as an enum in the service custom identifier portion.

### Service CIDs

| Function         | SRV CID | Content request            | Content response                                               | Note                                               |
| ---------------- | ------- | -------------------------- | -------------------------------------------------------------- | -------------------------------------------------- |
| Discover         | 0       | SN (from node)             | SN (of node) + ID (from core)                                  |                                                    |
| Ping             | 1       | -                          | -                                                              | Empty packet                                       |
| Identify         | 2       | Bool                       | -                                                              | True = blink red led fast, false = leave led alone |
| Device type      | 3       | -                          | Device type enum                                               |                                                    |
| Serial number    | 4       | -                          | SN                                                             |                                                    |
| Software version | 5       | -                          | Software version                                               |                                                    |
| Capability       | 6       | -                          | Capability bitfield                                            |                                                    |
| Read Name        | 7       | -                          | Name                                                           |                                                    |
| Set Name         | 8       | Name                       | Name                                                           | Respond only if requested                          |
| Uptime           | 9       | -                          | Uptime (uint32)                                                | ms                                                 |
| Loop Time        | 10      | -                          | Average and Maximum loop time (2x Number)                      | ms                                                 |
| Time sync        | 11      | Time sent                  | Original time sent, Local time recieved, Local time reply sent |                                                    |
| Set time offset  | 12      | Time offset (int32)        | -                                                              | ms                                                 |
| SNDB Read All    | 13      | -                          | Fragmentation, SN + ID (stream)                                | Core only                                          |
| SNDB Read        | 14      | ID or SN (based on length) | SN + ID                                                        | Core only                                          |
| SNDB Write       | 15      | SN + ID                    | SN + ID                                                        | Core only, setting ID to 0 works as delete         |
Device name is stored in standalone file to allow persistence.
If possible, the device name is shown in BLE advertising and on USB.

Every core implements SN Database.
The SNDB has it's own file, which stores (persistent) Serial number + ID pairs

Core also provides time sync reference. After ID assignment, the newly discovered device sends a single initial timesync packet to the core to sync it's time.

The core then randomly syncs other devices if idle.