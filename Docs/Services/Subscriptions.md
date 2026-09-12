A subscription service, initiated by the requester.
#### Table header

| Number of subscriptions | Entry 0 offset | Entry 0 size | Entry 1 offset | ... |
| ----------------------- | -------------- | ------------ | -------------- | --- |
| uint32                  | uint16         | uint16       | ...            | ... |
### Requester
Use define USE_SUB_REQUEST.
Subscriptions are stored in a sequential table, sorted by address.
#### Table entry

| Name                        | Size              | Note                                  |
| --------------------------- | ----------------- | ------------------------------------- |
| Address                     | uint16            | Provider                              |
| TRID                        | uint16            | Non-persistent                        |
| Target register             | 32bit (BlockInfo) | Local write, sets foreign origin flag |
| Source register             | 32bit (BlockInfo) | At provider's register                |
| Trigger type                | enum (8bit)       |                                       |
| Trigger settings and states | Flexible          | Trigger type dependent                |

Callback finds the target register based on the (TR)ID, and writes it there if possible, adding the foreign origin flag.

Re-activates after boot, recalls values from a file, which is a 1:1 copy of the table header followed by the entries.
### Provider
Use define USE_SUB_PROVIDE.
Active until canceled, information is not persistent on this side.
Sending device keeps an internal table of active subscriptions, maximum number is limited by device's RAM.
The table header is the same, but maximum number of entries/table size can be limited.
Updates are triggered on write with the subscription flag (deltas, changes, edges) or by time checked from the main loop (periodic).
#### Table entry

| Name                        | Size              | Note                   |
| --------------------------- | ----------------- | ---------------------- |
| Address (of requester)      | uint16            | 0 is invalid entry     |
| TRID                        | uint16            |                        |
| Source register             | 32bit (BlockInfo) |                        |
| Trigger type                | enum (8bit)       |                        |
| Trigger settings and states | Flexible          | Trigger type dependent |
### Trigger types
- Periodic (low priority)
	- Just send when timer runs out
	- Settings and states:
		- Period (ms)
		- Last time sent (ms)
- On change with period (low priority)
	- Pure bit comparision
	- Sends sooner if value changes, but not sooner than minimum time
	- Settings and states:
		- Period (ms), Minimum interval (ms)
		- Last time sent (ms)
- On change with confirmation (high priority)
	- Pure bit comparision
	- Sends if value changes, repeats until confirmed.
	- Value sent gets always updated.
	- Settings and states:
		- Retry interval (ms)
		- Last confirmed value, Last time sent (ms)
- Edge trigger / Rising edge / Falling edge (high priority)
	- Bool only.
	- Uses counter to convey correct amount, repeats until confirmed.
	- Output on the requester side is still bool (edge did/ did not happen for that loop)
	- Settings and states:
		- Retry interval (ms)
		- Incremental counter, Last time sent (ms)
- Delta (with period, low priority)
	- Integer/Number/Vector only (comparision with tolerance)
	- Sends sooner if value changes by more than tolerance, but not sooner than minimum interval allows
	- Settings and states:
		- Period (ms), Minimum interval (ms), Tolerance
		- Last time sent (ms), Last reported value
- Bound (with period, low priority)
	- Integer/Number only
	- Returns bool (or array of bools), true = value larger than bound, false = inverse
	- Settings and states:
		- Period (ms), Minimum interval (ms), Bounding value
		- Last time sent (ms), Last value
- Crossing Up/Down/Any (high priority)
	- Integer/Number only
	- Requires confirmation
	- Sends if the value crosses the bound in specified direction
	- Settings and states:
		- Retry interval (ms), Bounding value
		- Incremental counter, Last time sent (ms), Last Value
### Commands (040x)

| Function                             | ID  | Content request                   | Content response                           | Note                                                                             |
| ------------------------------------ | --- | --------------------------------- | ------------------------------------------ | -------------------------------------------------------------------------------- |
| Value update (provider to requester) | 0   | Last value (confirmation)         | Current value (TLFV)                       | Sent as response packet, request is confirmation if needed (inverted to normal). |
| Change subscription                  | 1   | Subscription information or Empty | Current value (TLFV)                       | Empty packet means cancel.                                                       |
| Get subscriptions (provider)         | 2   | Index or Empty                    | Provider table entry or Number of entries  |                                                                                  |
| Get subscriptions (requester)        | 3   | Index or Empty                    | Requester table entry or Number of entries |                                                                                  |
| Set subscription                     | 4   | Index + Entry of requester table  | -                                          | Index only means delete                                                          |


