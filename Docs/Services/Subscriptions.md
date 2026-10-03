A subscription service, initiated by the requester. Timeout 120s, renewed with new request.
Has a reserved TRID range, management by requester table.
#### Subscription table
A minimum describing record, used for in-between device request.

| Name               | Size              | Note                   |
| ------------------ | ----------------- | ---------------------- |
| Source register    | 32bit (BlockInfo) | At provider's register |
| Trigger type       | enum (8bit)       |                        |
| Minimum/Retry time | uint24            | ms                     |
| Period             | uint32            | ms                     |
| Deadzone           | Number (32bit)    | For number/vector      |
### Requester
Use define USE_SUB_REQUEST.
Subscriptions are stored in a sequential table, sorted by TRID.
#### Requester table entry

| Name                | Size              | Note                                                            |
| ------------------- | ----------------- | --------------------------------------------------------------- |
| Address of provider | uint16            |                                                                 |
| TRID                | uint16            |                                                                 |
| Subscription table  | ... 16 bytes      |                                                                 |
| Target register     | 32bit (BlockInfo) | Local write                                                     |
| Timeout             | uint32            | Local uptime, ignored by Set Subscription and isn't persistent. |

On recieving a value update finds the target register based on the TRID, and writes it there if possible.

Re-activates after boot, recalls values from a file, which is a 1:1 copy of the table except timeout value.
Non-requested value updates get a cancel request sent back.
### Provider
Use define USE_SUB_PROVIDE.
Active until canceled, information is not persistent on this side.
Sending device keeps a table of active subscriptions, maximum number is limited by device's RAM.
Updates are checked by a function from the main loop.
Ordering not specified.
#### Provider table entry

| Name                 | Size         | Note                                       |
| -------------------- | ------------ | ------------------------------------------ |
| Address of requester | uint16       | 0 is invalid entry                         |
| TRID                 | uint16       |                                            |
| Subscription table   | ... 16 bytes |                                            |
| Last time sent       | uint32       | ms in local uptime                         |
| Hash/Hashlike        | uint32       | Last confirmed                             |
| Timeout              | uint32       | Local uptime, ignored by Set Subscription. |
Hash/Hashlike:
- Hash - FNV-1a
- Counter - for Bool edges only
- Last value - Number/Integer only
- Subresolution Vector - Zoomed in portion of the sensitive area. 
  Example for 3D: 10 bits per axis = 5 bits above deadzone (Hash & Sign) + 5 bits below (Distance measurement)
  Not included if device does not measure any Vectors and does not have Script support.
### Trigger types
- None (canceled/to be deleted)
- Periodic (low priority, any type)
	- Just sends when timer runs out
	- Settings and states:
		- Period (ms)
		- Last time sent (ms)
- On change with period (low priority, any type)
	- Checks hash
	- Sends sooner if value changes, but not sooner than minimum interval.
	- Settings and states:
		- Period (ms), Minimum interval (ms)
		- Last time sent (ms), Hash
- On change with confirmation (high priority, any type)
	- Pure bit comparision
	- Sends if value changes, repeats (and updates) until confirmed, but not sooner than retry interval. Updates hash on confirmation.
	- Settings and states:
		- Retry interval (ms)
		- Last time sent (ms), Hash
- Rising/Falling/Any Edge detection (high priority, bool only)
	- Counts edges, sends update on increase
	- Settings and states:
		- Retry interval (ms)
		- Last time sent (ms), Counter
- Delta with period (low priority, scalar/vector only)
	- Checks distance (euclidian for vectors)
	- Sends sooner if threshold is reached, but not sooner than minimum interval.
	- Settings and states:
		- Period (ms), Minimum interval (ms)
		- Last time sent (ms), Last scalar value / Subresolution vector
### Inter-device commands (040x)

| Function                             | ID  | Content request                                     | Content response | Note                                                                             |
| ------------------------------------ | --- | --------------------------------------------------- | ---------------- | -------------------------------------------------------------------------------- |
| Value update (provider to requester) | 0   | Hash/Hashlike of last recieved value (confirmation) | Current value    | Sent as response packet, request is confirmation if needed (inverted to normal). |
| Change subscription                  | 1   | Subscription table                                  | Current value    | None type means cancel/delete. Sent once for deletion.                           |
### Requester commands (041x)

| Function                 | ID  | Content request          | Content response                        | Note                    |
| ------------------------ | --- | ------------------------ | --------------------------------------- | ----------------------- |
| Get subscriptions        | 0   | -                        | Fragmentation, Requester table (stream) |                         |
| Set subscription         | 1   | Entry of requester table | -                                       | None type means cancel. |
| Recall all subscriptions | 2   | -                        | Success                                 |                         |
| Save all subscriptions   | 3   | -                        | Success                                 |                         |
### Provider commands (042x)

| Function          | ID  | Content request         | Content response                       | Note                    |
| ----------------- | --- | ----------------------- | -------------------------------------- | ----------------------- |
| Get subscriptions | 0   | -                       | Fragmentation, Provider table (stream) |                         |
| Set subscription  | 1   | Entry of provider table | -                                      | None type means cancel. |
