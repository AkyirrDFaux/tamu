Non-USB devices are targets, USB/BLE devices are acting like hardware passthrough for the app.

#### Non-USB (Node/Router)
Self-contained, never overwritten, works with direct RS-Bus acting as half-duplex UART.
Completely minimal implementation, as little flash as possible, separate flash segment.

Enter:
- Button held at boot, the device sends it's serial number periodically (1s) until confirmed.
- If button was not pressed at boot, continue to node's app as usual.
Leave:
- By manual restart (press reset button).

Blink red when writing/reading flash, blink white periodically (50% duty, does not need to be precise) to indicate bootloader mode.
#### USB (Core)
Switch controls if the core device's mode. In bootloader mode, device stops any other service except this one (and obviously app connection/CLI). It gives over it's main (first) RS-Bus to work as half-duplex UART.

| Function          | SRV CID | Content request                                    | Content response                                   | Note                                           |
| ----------------- | ------- | -------------------------------------------------- | -------------------------------------------------- | ---------------------------------------------- |
| Bootloader check  | 0       | -                                                  | bool (True = ready, false = not)                   |                                                |
| Bootloader switch | 1       | bool (True to enter, false to leave)               | -                                                  |                                                |
| Device info       | 2       | -                                                  | SN                                                 | Null if not connected or device not enumerated |
| Bootloader Reader | 3       | uint32 Fragment Index                              | uint32 Fragment Index + Binary (256 byte fragment) | Wait for response                              |
| Bootloader Writer | 4       | uint32 Fragment Index + Binary (256 byte fragment) | uint32 Fragment Index                              | Wait for response                              |
The writer sends the Index and payload to the device. It responds when it is written.
Reader requests the specific blocks to be verified, the device reads it from flash.

Verification is done app-side (checksum, completeness), and it controls the whole operation.

On half duplex UART using control characters... request and response pair control characters Payload always 4, 14 or 260 bytes.
- "E" - enumeration
- "C" - confirm connection
- "W" - write
- "D" - done (writing)
- "V" - verify (read request)
- "R" - read (from flash)
#### Flow
- Device sends enumeration
- Core confirms
- Upload
	- Bootloader on core sends (on app's request) -> Device recieves
	- Device writes to flash, app waits
	- Device responds with Fragment index -> Bootloader on core recieves (reports to app)
	- ... repeat until done
- Verification
	- Bootloader on core sends request (from app) -> Device recieves
	- Device reads flash, prepares response, app waits
	- Device responds with Fragment index and read flash -> Bootloader on core recieves (reports to app)
	- App verifies
		- If okay, continue
		- If mismatch, correct it, verify again from start.
- If verified done