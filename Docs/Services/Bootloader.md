Allows the app to replace the main binary. The bootloader section and the app section must be aligned to 32 bytes. Implemented per device.

The bootloader is entered by holding a button during boot, and lights the white LED permanently. Otherwise the main app is launched.

It is as rudimentary and flash-saving as possible, and ignores the standard packets. USB devices accept the packets directly over USB, and cores additionally have a passthrough command to RSBus. The remaining devices accept the packets over [[RSBus and Packets|RSBus]], where other devices are expected not to interfere.

Timing and verification of the writes are the app's responsibility, as no response is given.
### Bootloader Packets
These packets are used on both RSBus and USB.

Write packet:

| Field        | Size    | Note |
| ------------ | ------- | ---- |
| Start Marker | uint8   | 0xCA |
| Padding      | 5bit    | 0    |
| Parity       | 1bit    | Even, covers command, offset and payload |
| Command      | 2bit    | 0b01 |
| Offset       | uint32  | Relative to the start of the main binary space |
| Payload      | uint8[32] | Data to be written |
| End Marker   | uint8   | 0xBC |

Read request packet:

| Field        | Size  | Note |
| ------------ | ----- | ---- |
| Start Marker | uint8 | 0xCA |
| Padding      | 5bit  | 0    |
| Parity       | 1bit  | Even, covers command and offset |
| Command      | 2bit  | 0b10 |
| Offset       | uint32 | Relative to the start of the main binary space |
| End Marker   | uint8 | 0xBC |

Read response packet:

| Field        | Size     | Note |
| ------------ | -------- | ---- |
| Start Marker | uint8    | 0xCA |
| Padding      | 5bit     | 0    |
| Parity       | 1bit     | Even, covers command, offset and payload |
| Command      | 2bit     | 0b11 |
| Offset       | uint32   | Relative to the start of the main binary space |
| Payload      | uint8[32] | Data read |
| End Marker   | uint8    | 0xBC |

The packets are sent whenever the USB or RSBus is clear, and all actions are blocking.
### Core Bootloader Passthrough Commands (002x)
Allows the app to send bootloader packets to the RSBus, for the read and write commands. Routers route the bootloader packets as well, acting as a broadcast; they are treated as high priority traffic on the same physical layer.

| Name              | ID | Request         | Response        | Note |
| ----------------- | -- | --------------- | --------------- | ---- |
| Send write packet | 0  | Offset, Payload | Success (bool)  |      |
| Request read      | 1  | Offset          | Offset, Payload |      |
