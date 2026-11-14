Allows the app to replace the main binary. The bootloader section, the app section and storage are aligned to the erase page size, and each of their sizes is a whole multiple of it.

The bootloader is entered by holding a button during boot, and lights the white LED permanently. Otherwise the main app is launched.

It is as rudimentary and flash-saving as possible, and ignores the standard packets. USB devices accept the packets directly over USB, and cores additionally have a passthrough command to RSBus. The remaining devices accept the packets over [[RSBus and Packets|RSBus]], where other devices are expected not to interfere.

Writes are acknowledged: every write request is answered by a write response carrying the offset of the last successfully written chunk, so the app can pipeline writes and resynchronise after a rejected chunk. A chunk that could not be written is answered with **the page it belongs to, bit 0 set**, so the app can re-erase and re-send the whole page rather than guess which chunk failed.

Erasing is a separate command from writing: a page is blanked by an erase request and then programmed by the write requests that cover it. A write never erases and does not require the page to have been erased first - a page that was never blanked simply receives wrong data, which the CRC verification and the read-back both catch.

Final verification of the image is the app's responsibility, in two measures: a CRC verify request over the whole app space, and a full read-back of the same space.
### Bootloader Packets
These packets are used on both RSBus and USB.

The command is a 4-bit field in the control byte. Bit 0 is `0` for a request and `1` for the response to that request; bits 1-3 select the operation. The four high bits of the control byte are the even parity bit (bit 4) and three padding bits (bits 5-7), always `0`.

| Command           | Value    |
| ----------------- | -------- |
| Identify request  | `0b0000` |
| Identify response | `0b0001` |
| Read request      | `0b0010` |
| Read response     | `0b0011` |
| Erase request     | `0b0100` |
| Erase response    | `0b0101` |
| Write request     | `0b0110` |
| Write response    | `0b0111` |
| CRC request       | `0b1000` |
| CRC response      | `0b1001` |

The range `0b1010`-`0b1111` is unassigned.

Identify request packet:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 4bit   | 0b0000 |
| Offset       | uint32 | 0 |
| End Marker   | uint8  | 0xBC |

Identify response packet:

| Field        | Size     | Note |
| ------------ | -------- | ---- |
| Start Marker | uint8    | 0xCA |
| Padding      | 3bit     | 0    |
| Parity       | 1bit     | Even, covers command, size, page size and name |
| Command      | 4bit     | 0b0001 |
| App size     | uint32   | Size of the uploadable app region in bytes. It is the app space alone on a device that partitions its flash, and also covers storage on a device without a partition table. |
| Name         | char[28] | Device name, null-padded |
| Page size    | uint32   | Erase page size in bytes. Every erase offset is a multiple of it, and the app size is a multiple of it. |
| End Marker   | uint8    | 0xBC |

Read request packet:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 4bit   | 0b0010 |
| Offset       | uint32 | Relative to the start of the main binary space |
| End Marker   | uint8  | 0xBC |

Read response packet:

| Field        | Size      | Note |
| ------------ | --------- | ---- |
| Start Marker | uint8     | 0xCA |
| Padding      | 3bit      | 0    |
| Parity       | 1bit      | Even, covers command, offset and payload |
| Command      | 4bit      | 0b0011 |
| Offset       | uint32    | Relative to the start of the main binary space |
| Payload      | uint8[32] | Data read |
| End Marker   | uint8     | 0xBC |

Erase request packet:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 4bit   | 0b0100 |
| Offset       | uint32 | Address of the page to blank. It must be a multiple of the page size reported by the identify response; any other offset is refused rather than rounded. |
| End Marker   | uint8  | 0xBC |

Erase response packet:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 4bit   | 0b0101 |
| Offset       | uint32 | The page that was blanked, or that page with bit 0 set when the erase failed |
| End Marker   | uint8  | 0xBC |

Write request packet:

| Field        | Size      | Note |
| ------------ | --------- | ---- |
| Start Marker | uint8     | 0xCA |
| Padding      | 3bit      | 0    |
| Parity       | 1bit      | Even, covers command, offset and payload |
| Command      | 4bit      | 0b0110 |
| Offset       | uint32    | Relative to the start of the main binary space |
| Payload      | uint8[32] | Data to be written |
| End Marker   | uint8     | 0xBC |

Write response packet:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 4bit   | 0b0111 |
| Offset       | uint32 | Offset of the last successfully written chunk; the page address with bit 0 set if a chunk in that page could not be written |
| End Marker   | uint8  | 0xBC |

CRC request packet:

The CRC always covers the whole app space - there is no offset and no length, so the frame carries neither.

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers the command |
| Command      | 4bit   | 0b1000 |
| End Marker   | uint8  | 0xBC |

CRC response packet:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers the command and the CRC |
| Command      | 4bit   | 0b1001 |
| CRC          | uint32 | CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, final XOR), little-endian, over the whole app space |
| End Marker   | uint8  | 0xBC |

The packets are sent whenever the USB or RSBus is clear, and all actions are blocking.
### Upload procedure
The app compares before it writes, so an update transfers only what changed:

1. Identify, to learn the app size and the page size.
2. For every page of the image: read it, compare with the image. Erase the page only where the two differ, and write only the chunks of pages that differ.
3. Blank the pages from the end of the image to the end of the app space. The CRC covers the whole app space, so that tail is part of what it measures; leaving an earlier image's bytes there makes the CRC unpredictable. The tail is a handful of pages (on the DAS, 9 of them).
4. CRC verify. Because the space is now a function of the image alone, the expected value is the host's CRC of the image padded to the app size with 0xFF.
5. Read the whole space back and compare it byte for byte.

Steps 3 and 4 are both required. The CRC is one exchange over the whole range; the read-back is the independent measure that does not depend on the device's CRC implementation being correct. A disagreement between them is a finding in its own right, not something to resolve by trusting one of them.

Loss on the wire is inherent and per-transaction, which is why step 2 matters: a page that already matches is never written, so an update that changes a few hundred bytes costs a handful of transmissions instead of one per 32 bytes.
### Core Bootloader Passthrough Commands (002x)
Allows the app to send bootloader packets to the RSBus, for the identification, write, read, erase and CRC commands. Routers route the bootloader packets as well, acting as a broadcast; they are treated as high priority traffic on the same physical layer.

The passthrough is asynchronous: the core issues the frame and keeps serving while it awaits the reply. A reply that does not arrive within 1 s returns the failure flag, and a request arriving while one is pending is refused.

The two packet families share one UART, so while a passthrough is outstanding the core tells them apart: the bytes of a bootloader frame accumulate in the passthrough's own buffer and everything else continues through the standard packet assembler. Neither is dropped, and a bootloader reply is never mistaken for - or discarded as - standard traffic.

| Name                   | ID | Request         | Response        | Note |
| ---------------------- | -- | --------------- | --------------- | ---- |
| Request identification | 0  | -               | App size, Page size, Name | |
| Send write packet      | 1  | Offset, Payload | Offset          | Success only if the offset matches the request |
| Request read           | 2  | Offset          | Offset, Payload |      |
| Request CRC            | 3  | -               | CRC             | CRC-32 over the whole app space |
| Erase page             | 4  | Offset          | Page            | Offset must be page-aligned |
