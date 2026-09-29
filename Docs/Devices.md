Use BOARD_DeviceName for code guarding device specific implementations.
defines TYPE_CORE, TYPE_ROUTER, TYPE_NODE
### Tamu v2.0A (ESP32-C3) - Core & Node

| Feature            | Pins                 | Note                                               |
| ------------------ | -------------------- | -------------------------------------------------- |
| RSBus 3V3          | TX 21, RX 20, TXEN 9 |                                                    |
| LED-Button         | 2                    | Contains pullup, button active low, LED active low |
| Gyr&Acc            | SDA 4, SCL 5         | LSM6DS3TR-C (0x6A), with 4k7 pullups               |
| Fan PWM Output     | 6, 10                |                                                    |
| LED Display Output | 0, 3                 |                                                    |
| White LED          | Missing hardware     | Ignore in implemetation                            |
| Unused exposed     | 1, 7, 8              |                                                    |
Services:
- Mandatory and Core
- CLI
- Dynamic memory
- Script
- Subscriptions (Request and Provide)
Modules: 
 - LED Display x2
 - Fan Output x2
 - Acc&Gyr
 - LED-Button
Page size: 4096 Bytes
Table size: 4 Pages 
Memory: ...a lot (MBs)
### DAS v0.1 (CH32V003) - Node

| Feature          | Pins                     | Note                                                       |
| ---------------- | ------------------------ | ---------------------------------------------------------- |
| RSBus 3V3        | TX PD5, RX PD6, TXEN PD4 |                                                            |
| LEDs             | Red PA1, White PD0       | Active high                                                |
| Button           | PC0                      | Requires pullup, active low                                |
| Range selector 1 | PA2 - PC7 - PD3          | 330Ohm - 10kOhm - 330kOhm, P-Channel switches (high = off) |
| Range selector 2 | PC1 - PC2 - PC3          | 330Ohm - 10kOhm - 330kOhm, P-Channel switches (high = off) |
| Measuring 1      | PD2 (A3)                 |                                                            |
| Measuring 2      | PC4 (A2)                 |                                                            |
Services:
- Mandatory services
- Subscriptions (Provide only)
- Uses reduced filesystem
Modules:
- Resistive measurement x2
- Button
- LED
Optimization cuts:
- Do not use TRID manager
	- TRID 0 is discovery
	- TRID 1 is timesync
	- TRID 2 is log/error
	- Packet handlers are fixed to the TRID
Page size: 64 Bytes (Fast mode)
Memory: 128B (single file from offset 0)

### Valu v2.0 (CH32V203G8R6) - Legacy standalone node with USB

| Feature           | Pins                                         | Note                             |
| ----------------- | -------------------------------------------- | -------------------------------- |
| LED-Button        | PA2                                          | Red, Active high                 |
| Buttons           | PB15, PB14, PB13                             | Requires pulldown, active high   |
| Fan PWM output    | PA8                                          |                                  |
| Measuring         | PA6 (ADC6), PA1 (ADC1), PA0 (ADC0)           | Reference resistor 10KOhm fixed. |
| LED strip outputs | PA14, PB8                                    |                                  |
| OLED Display      | MOSI PA7, CLK PA5, CS PA4, DRST PA3, DDC PB0 |                                  |
| I2C bus           | SCL PB6, SDA PB7                             |                                  |
| UART              | RX PA10, TX PA9                              |                                  |
Services:
- Mandatory
- App interface (USB)
- Dynamic memory
- Script
Modules: 
 - LED Display x2
 - Fan Output x1
 - LED-Button
 - Button x3
 - OLED Display (TODO)
 - Resistive measurement x3
Page size: 256 Bytes (Fast mode)
Table size: 2 Pages 
Memory: 8kB