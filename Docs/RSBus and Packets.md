# RSBus
RS485 Bus with CSMA/CD, 460.8kbaud.
Supply voltage for device 3.3V or directly from USB PD (9-20V).
Tree structure, no time slots or master, send when silent.
## Network structure
The core is the main device in each network. It assigns adresses.
Routers must be oriented with their main bus towards the core.

Routers's other busses are considered branches. Another router may be on the branch, but they must always "point" in the direction of the core.

No nodes allowed on busses crossing nets (to prevent discovery issues), only routers may connect using their branch busses.
### Sender
The transmission can start if the line has been silent for the time-equivalent of:
	8 bytes + (Packet priority/8) bytes + Random small delay (0-3 bytes).
	
The sent data is immediately verified while sending (a feedback loop).
If a collision happens, stop the transmission and wait for silence with a new random delay.

Before the start of the transmission a start/sync byte 0xAA is sent.
### Reciever
A simple state machine. 
Start recieving if 0xAA is recieved, the sync byte itself is dropped. Then if the target ID does not match the device as target (including broadcasts, and invalid when not assigned yet), discard it and wait for another start.

Checking of the packet validity is done separately from the main loop via `ProcessBus` function.

# Packets

| Section | Field          | Size          | Note                                                                                         |
| ------- | -------------- | ------------- | -------------------------------------------------------------------------------------------- |
| Generic | CRC8           | uint8         | covers everything after, calculated only when leaves device, checked on entering device only |
|         | Flags          | 8 bits        |                                                                                              |
|         | Reserved       | 4 bits        |                                                                                              |
|         | Priority       | 4 bits        | 0 = highest, default 8                                                                       |
|         | Payload Length | uint8         | in bytes                                                                                     |
| Routing | SRC ID         | uint16        | Source device's address                                                                      |
|         | TGT ID         | uint16        | Target device's address                                                                      |
|         | CMD            | uint16        | Command                                                                                      |
|         | TRID           | uint16        | Transaction ID                                                                               |
| Payload |                | max 116 bytes | Flexible size, command specific.                                                             |
- Flags:
	- REQACK (request response)
	- START (first)
	- STOP (last)
	- TYPE (Request/Response)
	- FRAG (first 4 payload bytes are fragmentation information, uint16 current frag. segment + uint16 total segments)
	- SUCCESS (response indicates success with no extra information)
	- FAIL (response indicates an error with no extra information)
- Priorities:
	- Errors (highest)
	- TimeSync packets
	- High priority subscriptions
	- Other
	- Low priority subscriptions
	- Streams
	- Logs (lowest)
Maximum length 128 bytes total, all devices have to handle it in full.
### Transaction IDs
Reserved ranges:

| Service       | Range           | Type                                 |
| ------------- | --------------- | ------------------------------------ |
| System, Logs  | 0x0000 - 0x0FFF | Incrementing, resets on overflow     |
| Subscriptions | 0x1000 - 0x1FFF | Table                                |
| Scripts       | ...             | Slot based (asynchronous operations) |
| App           | 0xF000 - 0xFFFF | Slot based (asynchronous operations) |
Each new outgoing request packet must have a transaction ID (repeats reuse it).
Responses arrive on the same TRID as the request.
The individual ranges are managed by the respective service, typically an incrementing counter or a slot/reserved table.
Allows for connecting asynchronous requests back to the correct function.
### Dispatcher
The packets are routed through the dispatcher between services and outside.
Internal packet:
 - Service (Sender)
 - Dispatcher
 - Service (Reciever)
External packet:
- Service (Sender)
- Dispatcher (Sender)
- RSBus (Sender)
- (optionally Routers)
- RSBus (Reciever)
- Dispatcher (Reciever)
- Service (Reciever)