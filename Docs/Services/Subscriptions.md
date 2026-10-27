A subscription service, initiated by the requester. A subscription provider times out an entry after 120 s and renews it on a new request; the requester keeps its row until the subscription is cancelled. The service owns a reserved TrID range and is managed by the requester table.
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
| Hash/Hashlike       | uint32       | Bookkeeping for this row, interpreted by its trigger: an **edge** trigger keeps the last **counter** it saw, the value triggers keep the last **value**. Wire only, not persisted, so a recall or a reboot starts the row with none. |

On receiving a value update, the requester finds the target register by its TrID and writes the value there if possible. The requester re-activates after boot and recalls its values from a file in [[Services/Storage]], a 1:1 copy of the table except for the timeout value. Non-requested value updates are answered with a cancel request.
#### Renewal
A subscription is kept alive by repeating the Set Subscription command (041x/1) with the same entry; there is no separate renewal command.

- Provider side: if the entry already exists, the command extends its timeout and answers success as usual. Nothing else about the entry changes — its hash, last value, last time sent and trigger state are left untouched.
- Requester side: it extends its own timeout when it receives a successful reply, and only then. A failed reply leaves the timeout alone.
- A requester entry is not removed because renewals failed; it stays and is retried until a renewal succeeds or it is cancelled.
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
| Hash/Hashlike        | uint32       | The reference the send decision compares against, per trigger: an **edge** trigger keeps a **counter** of counted level changes (and that counter is the update's payload), a **delta** trigger keeps the last **sent value**, and a confirm trigger keeps the **hash** of the confirmed value. Reused, not a value of its own; never written to the target. |
| Timeout              | uint32       | Local uptime, ignored by Set Subscription  |

The Hash/Hashlike field carries one of:

- Hash: FNV-1a
- Counter: for `Bool` edges only
	- 16-bit, packed in the 32-bit field: **low half = local counter** (what an update carries; wraps at 65536, parity keeps the level), **high half = last confirmed counter**. `high != low` means the last update is unacknowledged, and the same counter is re-sent until they match. A new edge increments the low half, a re-send never does.
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
	- Sends an update on each edge increase; **the payload is the counter itself** (`uint16`, little-endian, = the `Hash/Hashlike` counter).
	- **Parity is the level:** odd = currently true/pressed, even = false/lifted. A jump of `n` means `n` edges.
	- On receiving an edge update the requester compares the counter with the last one it saw to learn **how many edges are new**, and writes the **output value** as a pulse: `true` for one loop per new edge, consecutive, then `false`.
	- So a burst of three edges reads as three consecutive `true` loops; a single press reads as one; a held button reads `true` for one loop and then `false`.
	- The counter is never written into the target. Parity describes the counter, not the target.
	- The requester writes the derived `Bool` into the target; the counter is never stored as a value.
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
| Set subscription         | 1  | Entry of requester table | Success flag                            | A None type means cancel. |
| Recall all subscriptions | 2  | -                        | Success flag                          |      |
| Save all subscriptions   | 3  | -                        | Success flag                          |      |
### Provider Commands (042x)
| Name              | ID | Request                 | Response                               | Note |
| ----------------- | -- | ----------------------- | -------------------------------------- | ---- |
| Get subscriptions | 0  | -                       | Fragmentation, Provider table (stream) |      |
| Set subscription  | 1  | Entry of provider table | Success flag                           | A None type means cancel. |
