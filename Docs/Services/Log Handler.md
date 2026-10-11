Handles logs and error reports from the entire device and sends them over the RSBus. One implementation serves all devices, extended by a database for cores.

The Log Struct:

| Name      | Size   | Note |
| --------- | ------ | ---- |
| Source    | uint32 | The whole block index: the block type, the instance and the 16-bit field/key specifier |
| Log code  | uint32 | Source-specific |
| Timestamp | uint32 | Synced |

The source names the block that emitted the entry, and the code means whatever that source says it means - the same principle as the script errors being instruction-specific. The former 8-bit log category and log specifics fields together become the code.

The service sends the log immediately, at the priority that corresponds to it.
### Log Database (Core)
A core keeps the errors in RAM, with the device of origin, the occurrence count and the latest timestamp. The database lives on the heap.

A log database entry:

| Name          | Size   | Note |
| ------------- | ------ | ---- |
| Source Device | uint16 |      |
| Count         | uint16 |      |
| Log Struct    | `Log Struct` |      |

Error logs are accessible through the service, and the app decodes them into readable text.
### Commands (020x)
| Name           | ID | Request                                                     | Response                                                   | Note |
| -------------- | -- | ----------------------------------------------------------- | ---------------------------------------------------------- | ---- |
| Error reporter | 0  | -                                                           | Log Struct                                                 | Sends the log to the local core (ID 0.1) |
| GetLogs        | 1  | -                                                           | Fragmentation, stream of LogDatabase entries, oldest first | Core only |
| ClearReadLogs  | 2  | Number of logs to be cleared, starting from oldest (uint32) | Confirmation                                               | Core only; reply only if needed |
