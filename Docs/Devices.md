Use BOARD_DeviceName for code guarding device specific implementations.
### Tamu v2.0A (ESP32-C3) - Core

| Feature            | Pins                 | Note                                 |
| ------------------ | -------------------- | ------------------------------------ |
| RSBus 3V3          | TX 21, RX 20, TXEN 9 |                                      |
| LED-Button         | 2                    | Contains pullup, button pulls down   |
| Gyr&Acc            | SDA 4, SCL 5         | LSM6DS3TR-C (0x6A), with 4k7 pullups |
| Fan PWM Output     | 6, 10                |                                      |
| LED Display Output | 0, 3                 |                                      |
| White LED          | Missing hardware     | Ignore in implemetation              |
| Unused exposed     | 1, 7, 8              |                                      |
Services:
- Mandatory and Core (including CLI)
- Dynamic and Keyed memory
- Script
Modules: 
 - LED Display x2
 - Fan Output x2
 - Acc&Gyr
Page size: 4096 Bytes
### DAS v0.1 (CH32V003) - Node

| Feature          | Pins                     | Note                      |
| ---------------- | ------------------------ | ------------------------- |
| RSBus 3V3        | TX PD5, RX PD6, TXEN PD4 |                           |
| LEDs             | Red PA1, White PD0       |                           |
| Button           | PC0                      | Requires pullup           |
| Range selector 1 | PA2 - PC7 - PD3          | 330Ohm - 10kOhm - 330kOhm |
| Range selector 2 | PC1 - PC2 - PC3          | 330Ohm - 10kOhm - 330kOhm |
| Measuring 1      | PD2 (A3)                 |                           |
| Measuring 2      | PC4 (A2)                 |                           |
Services:
- Mandatory services + bootloader
Modules:
- Resistive measurement x2
- Button
Page size: 64 Bytes (Fast mode)