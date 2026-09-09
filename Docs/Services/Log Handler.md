Handles logs or error reports from the entire device, sends them over RS bus.
One implementation for all devices, extended by database for cores.
Log Struct:

| Source (BlockType+Instance) | Log Category | Log Specifics | Timestamp (synced) |
| --------------------------- | ------------ | ------------- | ------------------ |
| 16 bit                      | 8 bit        | 8 bit         | 32bit              |
The log handler service sends the log imediately with corresponding priority.

### Log database (Core)

If the device is core, it keeps the errors in RAM, with the device of origin, occurance count, and latest timestamp. Database is on heap.
Log Database Entry:

| Source Device | Count  | Log Struct |
| ------------- | ------ | ---------- |
| 16bit         | 16 bit | 64 bit     |

Errors logs are accessible via the service, app should be able to decode into readable text.

### Commands (020x)

| Function       | CID | **Payload In**                                              | **Payload out**                                            | **Note**                         |
| -------------- | --- | ----------------------------------------------------------- | ---------------------------------------------------------- | -------------------------------- |
| Error reporter | 0   | -                                                           | Log Struct                                                 | Sends Log to local core (ID 0.1) |
| GetLogs        | 1   | -                                                           | Fragmentation, Stream of LogDatabase entries, oldest first | Core only.                       |
| ClearReadLogs  | 2   | Number of logs to be cleared, starting from oldest (uint32) | Confirmation                                               | Core only. Reply only if needed. |
