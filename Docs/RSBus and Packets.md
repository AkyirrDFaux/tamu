RS485 bus with CSMA/CD at 460.8k baud. Supply voltage for a device is 3.3 V, or directly from USB PD (9 V to 20 V). The topology is a tree, with no time slots and no master: a device sends when the line is silent.
### Network Structure
The core is the main device in each network, and it assigns addresses. Routers are oriented with their main bus towards the core.

A router's other buses are branches. Another router may sit on a branch, but routers always point towards the core.

No nodes are allowed on buses crossing nets, which prevents discovery issues; only routers may connect through their branch buses.
#### Sender
A transmission may start when the line has been silent for the time equivalent of:

	8 bytes + (packet priority / 8) bytes + a small random delay of 0 to 3 bytes.

The sent data is verified while it is being sent, as a feedback loop. If a collision happens, the transmission stops and the sender waits for silence with a new random delay.

A start/sync byte, `0xAA`, is sent before the transmission begins.
#### Receiver
A simple state machine. It starts receiving when `0xAA` arrives, and the sync byte itself is dropped. If the target ID does not match the device, including broadcasts and invalid addresses when unassigned, the packet is discarded and the receiver waits for another start.

Packet validity is checked separately from the main loop, through the `ProcessBus` function.
### Packets
| Section | Part           | Size          | Note |
| ------- | -------------- | ------------- | ---- |
| Generic | CRC8           | uint8         | Covers everything after; calculated only when leaving the device, checked only on entering it |
|         | Flags          | 8bit          |      |
|         | Reserved       | 4bit          |      |
|         | Priority       | 4bit          | 0 is the highest, default 8. |
|         | Payload Length | uint8         | In bytes |
| Routing | SRC ID         | uint16        | Source device's address |
|         | TGT ID         | uint16        | Target device's address |
|         | CMD            | uint16        | Command |
|         | TrID           | uint16        | Transaction ID |
| Payload |                | uint8[116] | Flexible size, command specific |

Flags:

	- REQACK, request a response
	- START, the first packet
	- STOP, the last packet
	- TYPE, request or response
	- FRAG, the first 4 payload bytes are fragmentation information: a `uint16` current fragment segment plus a `uint16` total segment count
	- SUCCESS, a response indicating success with no extra information
	- FAIL, a response indicating an error with no extra information

Priorities, highest first:

	- Errors
	- TimeSync packets
	- High priority subscriptions
	- Other
	- Low priority subscriptions
	- Streams
	- Logs

The maximum length is 128 bytes in total, and all devices handle it in full.
#### Transaction IDs
Reserved ranges:

| Service | Range           | Type |
| ------- | --------------- | ---- |
| System, Logs | 0x0000-0x0FFF | Incrementing, resets on overflow |
| [[Services/Subscriptions|Subscriptions]] | 0x1000-0x1FFF | Table |
| [[Services/Script|Scripts]] | ... | Slot based, asynchronous operations |
| App | 0xF000-0xFFFF | Slot based, asynchronous operations |

Each new outgoing request packet carries a transaction ID, and repeats reuse it. Responses arrive on the same TrID as the request.

The individual ranges are managed by the respective service, typically as an incrementing counter or a slot table.

This connects asynchronous requests back to the correct function.
#### Dispatcher
Packets are routed through the dispatcher, between services and the outside.

An internal packet:

	- Service (sender)
	- Dispatcher
	- Service (receiver)

An external packet:

	- Service (sender)
	- Dispatcher (sender)
	- RSBus (sender)
	- Routers, optionally
	- RSBus (receiver)
	- Dispatcher (receiver)
	- Service (receiver)
