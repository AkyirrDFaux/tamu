A subscription service, initiated by the requester. A subscription times out after 120 s and is renewed by a new request. The service owns a reserved TrID range and is managed by the requester table.
#### Subscription Table
A minimum describing record, used for in-between device requests.

| Name               | Size           | Note                       |
| ------------------ | -------------- | -------------------------- |
| Source register    | `BlockInfo`    | At the provider's register |
| Trigger type       | `Enum` (8bit)  |                            |
| Minimum/Retry time | uint24         | ms                         |
| Period             | uint32         | ms                         |
| Deadzone           | `Number` or `uint32` | For `Number`, `Index`, `Uint32` and `Vector`; the type follows the value |
### Requester
Use define `USE_SUB_REQUEST`.

Subscriptions are stored in a sequential table, sorted by TrID.
#### Requester Table Entry
| Name                | Size         | Note |
| ------------------- | ------------ | ---- |
| Address of provider | uint16       |      |
| TrID                | uint16       |      |
| Subscription table  | ... 16 bytes |      |
| Target register     | `BlockInfo`  | Local write |
| Timeout             | uint32       | Local uptime; ignored by Set Subscription and not persistent |

On receiving a value update, the requester finds the target register by its TrID and writes the value there if possible. The requester re-activates after boot and recalls its values from a file in [[Services/Storage]], a 1:1 copy of the table except for the timeout value. Non-requested value updates are answered with a cancel request.
### Provider
Use define `USE_SUB_PROVIDE`.

Active until cancelled; the information is not persistent on this side. The sending device keeps a table of active subscriptions, with the maximum number limited by the device's RAM. Updates are checked by a function from the main loop, and the ordering is not specified.
#### Provider Table Entry
| Name                 | Size         | Note                                       |
| -------------------- | ------------ | ------------------------------------------ |
| Address of requester | uint16       | 0 is an invalid entry.                      |
| TrID                 | uint16       |                                            |
| Subscription table   | ... 16 bytes |                                            |
| Last time sent       | uint32       | ms in local uptime                         |
| Hash/Hashlike        | uint32       | Last confirmed                             |
| Timeout              | uint32       | Local uptime, ignored by Set Subscription  |

The Hash/Hashlike field carries one of:

- Hash: FNV-1a
- Counter: for `Bool` edges only
- Last value: for `Number`, `Index` and `Uint32` values only
- Subresolution Vector: a zoomed-in portion of the sensitive area. For 3D it is 10 bits per axis, that is 5 bits above the deadzone (Hash and Sign) plus 5 bits below (Distance measurement). It is not included if the device measures no Vectors and has no Script support.
### Trigger Types
- None (cancelled or to be deleted)
- Periodic (low priority, any type)
	- Sends when the timer runs out.
	- Settings and states:
		- Period (ms)
		- Last time sent (ms)
- On change with period (low priority, any type)
	- Checks the hash.
	- Sends sooner if the value changes, but never sooner than the minimum interval.
	- Settings and states:
		- Period (ms), Minimum interval (ms)
		- Last time sent (ms), Hash
- On change with confirmation (high priority, any type)
	- Pure bit comparison.
	- Sends if the value changes, repeats and updates the hash until the value is confirmed, but never sooner than the retry interval.
	- Settings and states:
		- Retry interval (ms)
		- Last time sent (ms), Hash
- Rising, falling or any edge detection (high priority, `Bool` only)
	- Counts edges and sends an update on increase.
	- Settings and states:
		- Retry interval (ms)
		- Last time sent (ms), Counter
- Delta with period (low priority, scalar or vector only)
	- Checks the distance, which is euclidean for vectors.
	- Sends sooner if the threshold is reached, but never sooner than the minimum interval.
	- Settings and states:
		- Period (ms), Minimum interval (ms)
		- Last time sent (ms), Last scalar value or Subresolution vector
### Inter-device Commands (040x)
| Name                                 | ID | Request                                             | Response      | Note |
| ------------------------------------ | -- | --------------------------------------------------- | ------------- | ---- |
| Value update (provider to requester) | 0  | Hash/Hashlike of last received value (confirmation) | Current value | Sent as a response packet; the request doubles as confirmation if needed, inverted from the normal direction. |
| Change subscription                  | 1  | Subscription table                                  | Current value | A None type means cancel or delete; sent once for deletion. |
### Requester Commands (041x)
| Name                     | ID | Request                  | Response                                | Note |
| ------------------------ | -- | ------------------------ | --------------------------------------- | ---- |
| Get subscriptions        | 0  | -                        | Fragmentation, Requester table (stream) |      |
| Set subscription         | 1  | Entry of requester table | -                                       | A None type means cancel. |
| Recall all subscriptions | 2  | -                        | Success (bool)                          |      |
| Save all subscriptions   | 3  | -                        | Success (bool)                          |      |
### Provider Commands (042x)
| Name              | ID | Request                 | Response                               | Note |
| ----------------- | -- | ----------------------- | -------------------------------------- | ---- |
| Get subscriptions | 0  | -                       | Fragmentation, Provider table (stream) |      |
| Set subscription  | 1  | Entry of provider table | -                                      | A None type means cancel. |
