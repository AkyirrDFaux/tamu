Allows the app to replace the main binary. The bootloader section, the app section and storage are aligned to the erase page size, and each size is a whole multiple of it.

Entered by holding a button during boot; it lights the white LED permanently, otherwise the app is launched. It is as rudimentary and flash-saving as possible, and ignores the standard packets. USB devices take the packets directly, cores add a passthrough to RSBus, and the other devices take them over [[RSBus and Packets|RSBus]].

Writes are acknowledged: a write is answered with the offset of the last successfully written chunk, so the app can pipeline and resynchronise after a rejection. A chunk that could not be written is answered with **its page address, bit 0 set**.

Erasing is separate from writing: an erase request blanks a page, then writes program it. A write never erases and does not require a prior erase - an unblanked page simply receives wrong data, which verification catches.

Verification is the app's job, in two measures: a CRC request over the whole app space, and a read-back of the same space.
### Bootloader Packets
Used on both RSBus and USB. A bootloader that does not report a page size in its identify response does not conform to this document and is refused by the app rather than guessed at.

The command is a 4-bit field in the control byte: bit 0 is `0` for a request and `1` for its response, bits 1-3 select the operation. The rest of the control byte is the even parity bit (bit 4) and three padding bits (bits 5-7), always `0`.

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

`0b1010`-`0b1111` are unassigned.

Identify request:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 4bit   | 0b0000 |
| Offset       | uint32 | 0 |
| End Marker   | uint8  | 0xBC |

Identify response:

| Field        | Size     | Note |
| ------------ | -------- | ---- |
| Start Marker | uint8    | 0xCA |
| Padding      | 3bit     | 0    |
| Parity       | 1bit     | Even, covers command, app size, page size and name |
| Command      | 4bit     | 0b0001 |
| App size     | uint32   | Uploadable app region in bytes: the app space alone on a partitioned device, and including storage where there is no partition table. |
| Page size    | uint32   | Erase page size in bytes. Erase offsets are multiples of it, and the app size is a multiple of it. |
| Name         | char[28] | Device name, null-padded |
| End Marker   | uint8    | 0xBC |

Read request:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 4bit   | 0b0010 |
| Offset       | uint32 | Relative to the start of the main binary space |
| End Marker   | uint8  | 0xBC |

Read response:

| Field        | Size      | Note |
| ------------ | --------- | ---- |
| Start Marker | uint8     | 0xCA |
| Padding      | 3bit      | 0    |
| Parity       | 1bit      | Even, covers command, offset and payload |
| Command      | 4bit      | 0b0011 |
| Offset       | uint32    | Relative to the start of the main binary space |
| Payload      | uint8[32] | Data read |
| End Marker   | uint8     | 0xBC |

Erase request:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 4bit   | 0b0100 |
| Offset       | uint32 | Address of the page to blank; must be a multiple of the page size. Any other offset is refused, never rounded. |
| End Marker   | uint8  | 0xBC |

Erase response:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 4bit   | 0b0101 |
| Offset       | uint32 | The blanked page, or that page with bit 0 set when the erase failed |
| End Marker   | uint8  | 0xBC |

Write request:

| Field        | Size      | Note |
| ------------ | --------- | ---- |
| Start Marker | uint8     | 0xCA |
| Padding      | 3bit      | 0    |
| Parity       | 1bit      | Even, covers command, offset and payload |
| Command      | 4bit      | 0b0110 |
| Offset       | uint32    | Relative to the start of the main binary space |
| Payload      | uint8[32] | Data to be written |
| End Marker   | uint8     | 0xBC |

Write response:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and offset |
| Command      | 4bit   | 0b0111 |
| Offset       | uint32 | Offset of the last successfully written chunk, or the page address with bit 0 set when a chunk in that page failed |
| End Marker   | uint8  | 0xBC |

CRC request. The CRC always covers the whole app space, so the frame carries no offset and no length:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers the command |
| Command      | 4bit   | 0b1000 |
| End Marker   | uint8  | 0xBC |

CRC response:

| Field        | Size   | Note |
| ------------ | ------ | ---- |
| Start Marker | uint8  | 0xCA |
| Padding      | 3bit   | 0    |
| Parity       | 1bit   | Even, covers command and CRC |
| Command      | 4bit   | 0b1001 |
| CRC          | uint32 | CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, final XOR), little-endian, over the whole app space |
| End Marker   | uint8  | 0xBC |

Packets are sent whenever USB or RSBus is clear; all actions are blocking.
### Upload procedure
1. Identify, for the app size and page size.
2. Per page: read it, compare with the image, and erase and write only the pages that differ.
3. Blank the pages from the end of the image to the end of the app space - the CRC covers that tail, so it must not hold an earlier image's bytes. The expected CRC is then the host's CRC of the image padded to the app size with `0xFF`.
4. CRC verify against it.
5. Read the whole space back and compare byte for byte.
6. If the read-back found mismatches, correct exactly the pages they fall in (erase and write only those) and repeat 4 and 5. Up to **five** correction rounds; a sixth failure ends the upload naming the offsets that disagree.

Steps 4 and 5 are independent: the CRC is one exchange whose value depends on the device's own CRC code being right, the read-back does not. A disagreement between them is a finding, not something to resolve by trusting either one.
### Core Bootloader Passthrough Commands (002x)
Lets the app send bootloader packets to the RSBus for all five operations. Routers treat them as a broadcast and as high-priority traffic on the same physical layer.

The passthrough is asynchronous: the core issues the frame and keeps serving while it awaits the reply.

A failure is never reported without saying which failure it was, because the app retries some and not others. Every failure answer carries a one-byte reason:

| Reason | Meaning |
| ------ | ------- |
| `0x01` | Refused: another passthrough request is outstanding |
| `0x02` | Refused: the request is malformed - payload too short or an unknown command ID |
| `0x03` | The node did not answer before the 1 s deadline |
| `0x04` | The node answered with a failure frame of its own |

`0x01` and `0x02` are decided by the core without touching the bus; `0x03` means the request went out and nothing came back; `0x04` carries the node's own answer semantics (an erase at a misaligned offset, a write into a page that was not erased).

Both packet families share one UART, so while a passthrough is outstanding the core separates them: a bootloader frame accumulates in the passthrough's own buffer and everything else continues through the standard packet assembler. Neither is dropped, and a reply is never mistaken for standard traffic.

The command IDs follow the bootloader command order - identify, read, erase, write, CRC - so a table row and its packet always carry the same rank.

| Name                   | ID | Request         | Response        | Note |
| ---------------------- | -- | --------------- | --------------- | ---- |
| Request identification | 0  | -               | App size, Page size, Name | |
| Request read           | 1  | Offset          | Offset, Payload | |
| Erase page             | 2  | Offset          | Page            | Offset must be page-aligned |
| Send write packet      | 3  | Offset, Payload | Offset          | Success only if the offset matches the request |
| Request CRC            | 4  | -               | CRC             | CRC-32 over the whole app space |
