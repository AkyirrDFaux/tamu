A subscription service, initiated by the requester.
#### Table header

| Number of subscriptions | Entry 0 offset | Entry 0 size | Entry 1 offset | ... |
| ----------------------- | -------------- | ------------ | -------------- | --- |
| uint32                  | uint16         | uint16       | ...            | ... |
### Requester
Subscription requester has reserved TRID range 0xFA00 - 0xFBFF, TRID is subscription's ID + 0xFA00.
Subscriptions are stored in a sequential table.
#### Table entry

| Name            | Size              | Note                                  |
| --------------- | ----------------- | ------------------------------------- |
| Target register | 32bit (BlockInfo) | Local write, sets foreign origin flag |
| Source register | 32bit (BlockInfo) | At provider's register                |
| Address         | uint16            | Provider                              |
| Trigger type    | enum              |                                       |
| Period          | uint32            | optional, ms                          |
| Minimum time    | uint32            | optional, ms                          |
| Counter         | uint32            | optional                              |
| Tolerance       | Flexible          | optiona, TLFV                         |
Callback finds the target register based on the (TR)ID, and writes it there if possible, adding the foreign origin flag.

Re-activates after boot, recalls values from a file, which is a 1:1 copy of the table header followed by the entries.
### Provider
Active until canceled, information is not persistent on this side.
Sending device keeps an internal table of active subscriptions, maximum number is limited by device's RAM.
The table header is the same, but maximum number of entries/table size can be limited.
#### Table entry

| Name                   | Size              | Note                        |
| ---------------------- | ----------------- | --------------------------- |
| TRID                   | uint16            |                             |
| Source register        | 32bit (BlockInfo) |                             |
| Address (of requester) | uint16            | 0 is invalid entry          |
| Trigger type           | enum              |                             |
| Period                 | uint32            | optional, ms, 0 means never |
| Last sent              | uint32            | optional, ms, from uptime   |
| Minimum time           | uint32            | optional, ms                |
| Counter                | uint32            | optional                    |
| Tolerance              | N                 | optional, TLFV              |
| Last Value             | N                 | optional, TLFV              |
### Trigger types
- Periodic (low priority)
	- Just send when timer runs out
- On change with period (low priority)
	- Pure bit comparision
	- Sends sooner if value changes, but not sooner than minimum time
- On change with confirmation (high priority)
	- Pure bit comparision
	- Sends if value changes, repeats until confirmed.
	- Value sent gets always updated.
- Edge trigger / Rising edge / Falling edge (high priority)
	- Bool only.
	- Uses counter to convey correct amount, repeats until confirmed.
	- Output on the requester side is still bool (edge did/ did not happen for that loop)
- Delta with period (low priority)
	- Integer/Number/Vector/Matrix only (comparision with tolerance)
	- Sends sooner if value changes, but not sooner than minimum time
- Delta with confimation (high priority)
	- Integer/Number/Vector/Matrix only (comparision with tolerance)
	- Sends if value changes beyond tolerance, repeats until confirmed.
	- Value sent gets updated if beyond tolerance until confirmed.
	- The threshold violation must be confirmed.
### Commands (040x)

| Function                             | ID  | Content request                   | Content response                           | Note                                                                             |
| ------------------------------------ | --- | --------------------------------- | ------------------------------------------ | -------------------------------------------------------------------------------- |
| Value update (provider to requester) | 0   | Last value (confirmation)         | Current value (TLFV)                       | Sent as response packet, request is confirmation if needed (inverted to normal). |
| Change subscription                  | 1   | Subscription information or Empty | Current value (TLFV)                       | Empty packet means cancel.                                                       |
| Get subscriptions (provider)         | 2   | Index or Empty                    | Provider table entry or Number of entries  |                                                                                  |
| Get subscriptions (requester)        | 3   | Index or Empty                    | Requester table entry or Number of entries |                                                                                  |
| Set subscription                     | 4   | Index + Entry of requester table  | -                                          | Index only means delete                                                          |


