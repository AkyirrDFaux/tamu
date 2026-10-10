Allows the app to replace the main binary. The bootloader section and the app section must be aligned to 32 bytes. Implemented per device.

The bootloader is entered by holding a button during boot, and lights the white LED permanently. Otherwise the main app is launched.

It is as rudimentary and flash-saving as possible, and ignores the standard packets. USB devices accept the packets directly over USB, and cores additionally have a passthrough command to RSBus. The remaining devices accept the packets over [[RSBus and Packets|RSBus]], where other devices are expected not to interfere.

Writes are acknowledged: every write request is answered by a write response carrying the offset of the last successfully written chunk, so the app can pipeline writes and resynchronise after a rejected chunk. Final verification of the image remains the app's responsibility, via read requests.
### Bootloader Packets
These packets are used on both RSBus and USB.

The command is a 3-bit field in the control byte:

| Command           | Value   |
| ----------------- | ------- |
| Identify request  | `0b000` |
| Identify response | `0b001` |
| Write request     | `0b010` |
| Write response    | `0b011` |
| Read request      | `0b100` |
| Read response     | `0b101` |

Identify request packet:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 4bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 3bit   | 0b000 |
| Offset       | uint32 | 0 |
| End Marker   | uint8  | 0xBC |

Identify response packet:

| Field        | Size     | Note |
| ------------ | -------- | ---- |
| Start Marker | uint8    | 0xCA |
| Padding      | 4bit     | 0    |
| Parity       | 1bit     | Even, covers command, size and name |
| Command      | 3bit     | 0b001 |
| App size     | uint32   | Size of the app space in bytes |
| Name         | char[32] | Device name, null-padded |
| End Marker   | uint8    | 0xBC |

Write request packet:

| Field        | Size      | Note |
| ------------ | --------- | ---- |
| Start Marker | uint8     | 0xCA |
| Padding      | 4bit      | 0    |
| Parity       | 1bit      | Even, covers command, offset and payload |
| Command      | 3bit      | 0b010 |
| Offset       | uint32    | Relative to the start of the main binary space |
| Payload      | uint8[32] | Data to be written |
| End Marker   | uint8     | 0xBC |

Write response packet:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 4bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 3bit   | 0b011 |
| Offset       | uint32 | Offset of the last successfully written chunk |
| End Marker   | uint8  | 0xBC |

Read request packet:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 4bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 3bit   | 0b100 |
| Offset       | uint32 | Relative to the start of the main binary space |
| End Marker   | uint8  | 0xBC |

Read response packet:

| Field        | Size      | Note |
| ------------ | --------- | ---- |
| Start Marker | uint8     | 0xCA |
| Padding      | 4bit      | 0    |
| Parity       | 1bit      | Even, covers command, offset and payload |
| Command      | 3bit      | 0b101 |
| Offset       | uint32    | Relative to the start of the main binary space |
| Payload      | uint8[32] | Data read |
| End Marker   | uint8     | 0xBC |

The packets are sent whenever the USB or RSBus is clear, and all actions are blocking.
### Core Bootloader Passthrough Commands (002x)
Allows the app to send bootloader packets to the RSBus, for the identify, read and write commands. Routers route the bootloader packets as well, acting as a broadcast; they are treated as high priority traffic on the same physical layer.

| Name                   | ID | Request         | Response        | Note |
| ---------------------- | -- | --------------- | --------------- | ---- |
| Send write packet      | 0  | Offset, Payload | Success flag    |      |
| Request read           | 1  | Offset          | Offset, Payload |      |
| Request identification | 2  | -               | App size, Name  |      |
