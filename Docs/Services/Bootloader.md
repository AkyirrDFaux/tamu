Allows for replacement of the main binary by the app.
Bootloader section and app section must be aligned to 32 bytes.
Implemented per-device.

Entered by holding a button on boot, lights up white LED permanently.
Otherwise the main app is launched.

As rudimentary and flash saving as possible, ignores standard packets.
USB devices accept the packets directly from USB for their bootloader.
Cores additionally the have a passthrough command to RSBus.
Remaining devices accept the packets via RSBus, other devices are expected not to interfere.

The app has to time and verify the writes correctly, as no response is given.
### Bootloader Packets
These packets are used on both RSBus and USB.

Write packet:

|              | Size    | Note                                  |
| ------------ | ------- | ------------------------------------- |
| Start Marker | 8bit    | 0xCA                                  |
| Padding      | 5bit    | 0                                     |
| Parity       | 1bit    | Even, covers command, offset, payload |
| Command      | 2bit    | 0b01                                  |
| Offset       | 32bit   | Relative to main binary space start.  |
| Payload      | 32bytes | Data to be written                    |
| End Marker   | 8bit    | 0xBC                                  |
Read request packet:

|              | Size  | Note                                 |
| ------------ | ----- | ------------------------------------ |
| Start Marker | 8bit  | 0xCA                                 |
| Padding      | 5bit  | 0                                    |
| Parity       | 1bit  | Even, covers command, offset         |
| Command      | 2bit  | 0b10                                 |
| Offset       | 32bit | Relative to main binary space start. |
| End Marker   | 8bit  | 0xBC                                 |
Read response packet:

|              | Size    | Note                                  |
| ------------ | ------- | ------------------------------------- |
| Start Marker | 8bit    | 0xCA                                  |
| Padding      | 5bit    | 0                                     |
| Parity       | 1bit    | Even, covers command, offset, payload |
| Command      | 2bit    | 0b11                                  |
| Offset       | 32bit   | Relative to main binary space start.  |
| Payload      | 32bytes | Data read                             |
| End Marker   | 8bit    | 0xBC                                  |
The packets are sent whenever the USB/RSBus is clear, all actions are blocking.
### Core bootloader passthrough commands (002x)
Allow the app to send bootloader packets to the RSBus (read and write command).
Routers route the bootloader packets as well (acts as broadcast), they are treated as different (high priority) traffic on same physical layer.

| Function          | ID  | Content request | Content response | Note |
| ----------------- | --- | --------------- | ---------------- | ---- |
| Send write packet | 0   | Offset, Payload | Success          |      |
| Request read      | 1   | Offset          | Offset, Payload  |      |
