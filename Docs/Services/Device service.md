This service can update ID, report device type, compiled software version, serial number, capability, uptime, and provides time synchronization to core.
One implementation for all devices, extended by SNDB for cores (also single implementation).

If device has core capability, it can assign IDs, and store them in the registry.
If the serial number is already stored, it assigns the same ID as before.

Else, it sends discoverery packets if no ID (0) was assigned to the device periodically and waits for a message with it's serial number.

Specific packet function is defined as an enum in the service custom identifier portion.

### Service CIDs

| Function         | SRV CID | Content request            | Content response                                               | Note                      |
| ---------------- | ------- | -------------------------- | -------------------------------------------------------------- | ------------------------- |
| Discover         | 0       | SN (from node)             | SN (of node) + ID (from core)                                  |                           |
| Ping             | 1       | -                          | -                                                              | Empty packet              |
| Device type      | 2       | -                          | Device type enum                                               |                           |
| Serial number    | 3       | -                          | SN                                                             |                           |
| Software version | 4       | -                          | Software version                                               |                           |
| Capability       | 5       | -                          | Capability bitfield                                            |                           |
| Read Name        | 6       | -                          | Name                                                           |                           |
| Set Name         | 7       | Name                       | Name                                                           | Respond only if requested |
| Uptime           | 8       | -                          | Uptime (uint32)                                                | ms                        |
| Loop Time        | 9       | -                          | Average and Maximum loop time (2x Number)                      | ms                        |
| Time sync        | 10      | Time sent                  | Original time sent, Local time recieved, Local time reply sent |                           |
| Set time offset  | 11      | Time offset (int32)        | -                                                              | ms                        |
| SNDB Read All    | 12      | -                          | SN + ID stream                                                 | Core only                 |
| SNDB Read        | 13      | ID or SN (based on length) | SN + ID                                                        | Core only                 |
| SNDB Write       | 14      | SN + ID                    | SN + ID                                                        | Core only                 |
Core implements SN Database
Has it's own file.
Stores (permanent) Serial number + ID pairs

Core also provides time sync, about once per (few) minutes, the time sync is performed at least 3 times with a delay of few seconds to provide an average
