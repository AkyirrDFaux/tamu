Handles logs or error reports from the entire device, sends them over RS bus.
One implementation for all devices, extended by database for cores.
Log Struct:

| Source is Service (0) or Block (1) | Source Block/Service ID | LogCode | Timestamp (synced) |
| ---------------------------------- | ----------------------- | ------- | ------------------ |
| 1 bit                              | 15 bit                  | 16 bit  | 32bit              |
Log code can be comprised of several segments, origin specific. Usually 8 bit x2 (category + specific).
The log handler service sends the log imediately.

If the device is core, it keeps the errors in RAM, with the device of origin, occurance count, and latest timestamp. Database is on heap.
Log Database Entry:

| Source Device | Count  | Log Struct |
| ------------- | ------ | ---------- |
| 16bit         | 16 bit | 64 bit     |

Errors logs are accessible via the service, app should be able to decode into readable text.

| Function       | CID | **Payload In**                                              | **Payload out**                             | **Note**                                                                                       |
| -------------- | --- | ----------------------------------------------------------- | ------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| Error reporter | 0   | -                                                           | Log Struct                                  | Outbound only for non-core devices (Log reporter). Core devices only inbound (Database input). |
| GetLogs        | 1   | -                                                           | Stream of LogDatabase entries, oldest first | Core only.                                                                                     |
| ClearReadLogs  | 2   | Number of logs to be cleared, starting from oldest (uint32) | Confirmation                                | Core only. Reply only if needed.                                                               |
