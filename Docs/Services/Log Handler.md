Handles logs and error reports from the entire device and sends them over the RSBus. One implementation serves all devices, extended by a database for cores.

The Log Struct:

| Name          | Size   | Note |
| ------------- | ------ | ---- |
| Source        | uint16 | BlockType and Instance |
| Log Category  | uint8  |      |
| Log Specifics | uint8  |      |
| Timestamp     | uint32 | Synced |

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
