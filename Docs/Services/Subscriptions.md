A subscription service, initiated by the requester.
### Requester
Use define USE_SUB_REQUEST.
Subscriptions are stored in a sequential table, sorted by address.
#### Table entry

| Name                | Size              | Note                                  |
| ------------------- | ----------------- | ------------------------------------- |
| Address of provider | uint16            | Provider                              |
| TRID                | uint16            | Non-persistent                        |
| Target register     | 32bit (BlockInfo) | Local write, sets foreign origin flag |
| Source register     | 32bit (BlockInfo) | At provider's register                |
| Trigger type        | enum (8bit)       |                                       |
|                     | 24bit padding     |                                       |
| Period              | uint32            |                                       |
| Minimum/Retry time  | uint32            |                                       |
Callback finds the target register based on the address and TRID, and writes it there if possible, adding the foreign origin flag.

Re-activates after boot, recalls values from a file, which is a 1:1 copy of the table.
### Provider
Use define USE_SUB_PROVIDE.
Active until canceled, information is not persistent on this side.
Sending device keeps a table of active subscriptions, maximum number is limited by device's RAM.
Updates are checked by a function from the main loop.
#### Table entry

| Name                   | Size              | Note               |
| ---------------------- | ----------------- | ------------------ |
| Address (of requester) | uint16            | 0 is invalid entry |
| TRID                   | uint16            |                    |
| Source register        | 32bit (BlockInfo) |                    |
| Trigger type           | enum (8bit)       |                    |
|                        | 24bit padding     |                    |
| Period                 | uint32            |                    |
| Minimum/Retry interval | uint32            |                    |
| Last time sent         | uint32            |                    |
| Hash                   | uint32            | FNV-1a             |

### Trigger types
- Periodic (low priority)
	- Just sends when timer runs out
	- Settings and states:
		- Period (ms)
		- Last time sent (ms)
- On change with period (low priority)
	- Checks hash
	- Sends sooner if value changes, but not sooner than minimum interval.
	- Settings and states:
		- Period (ms), Minimum interval (ms)
		- Last time sent (ms), Hash
- On change with confirmation (high priority)
	- Pure bit comparision
	- Sends if value changes, repeats (and updates) until confirmed, but not sooner than retry interval. Updates hash on confirmation.
	- Settings and states:
		- Retry interval (ms)
		- Last time sent (ms), Hash
### Commands (040x)

| Function                             | ID  | Content request                            | Content response                           | Note                                                                             |
| ------------------------------------ | --- | ------------------------------------------ | ------------------------------------------ | -------------------------------------------------------------------------------- |
| Value update (provider to requester) | 0   | Hash of last recieved value (confirmation) | Current value                              | Sent as response packet, request is confirmation if needed (inverted to normal). |
| Change subscription                  | 1   | Subscription information or Empty          | Current value                              | Empty packet means cancel.                                                       |
| Get subscriptions (provider)         | 2   | Index or Empty                             | Provider table entry or Number of entries  |                                                                                  |
| Get subscriptions (requester)        | 3   | Index or Empty                             | Requester table entry or Number of entries |                                                                                  |
| Set subscription                     | 4   | Index + Entry of requester table           | -                                          | Index only means delete                                                          |


