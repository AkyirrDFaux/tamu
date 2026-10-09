The devices the firmware targets, with their pins, services, modules and storage geometry. Device-specific implementations are guarded with `BOARD_DeviceName`.

Use define `TYPE_CORE`, `TYPE_ROUTER` or `TYPE_NODE`.
### Device Types
| Device             | Type |
| ------------------ | ---- |
| Unknown            | 0x00 |
| Tamu v2.0A         | 0x01 |
| Valu v2.0          | 0x02 |
| Dual Analog Sensor | 0x03 |
### Tamu v2.0A (ESP32-C3), Core and Node
| Feature            | Pins                 | Note |
| ------------------ | -------------------- | ---- |
| RSBus 3V3          | TX 21, RX 20, TXEN 9 |      |
| LED-Button         | 2                    | Contains a pull-up; button active low, LED active low |
| Gyr & Acc          | SDA 4, SCL 5         | LSM6DS3TR-C (0x6A), with 4k7 pull-ups |
| Fan PWM Output     | 6, 10                |      |
| LED Display Output | 0, 3                 |      |
| White LED          | Missing hardware     | Ignore in the implementation; the bootloader's boot signal has nothing to drive |
| Unused exposed     | 1, 7, 8              |      |
Services:

- Mandatory and Core
- USB Bootloader and passthrough, see [[Services/Bootloader]]
- Dynamic memory, under [[Services/Register]]
- [[Services/Script|Script]]
- [[Services/Subscriptions|Subscriptions]], request and provide

Modules:

- LED Display, 2 instances
- Fan Output, 2 instances
- Acc & Gyr
- LED-Button

Page size: 4096 bytes
Table size: 4 pages
Storage: ... a lot (MBs)
### DAS v0.1 (CH32V003), Node
| Feature          | Pins                     | Note |
| ---------------- | ------------------------ | ---- |
| RSBus 3V3        | TX PD5, RX PD6, TXEN PD4 |      |
| LEDs             | Red PA1, White PD0       | Active high; the white LED is the bootloader indicator |
| Button           | PC0                      | Requires a pull-up; active low |
| Range selector 1 | PA2 - PC7 - PD3          | 330 Ohm, 10k Ohm and 330k Ohm, P-channel switches (high is off) |
| Range selector 2 | PC1 - PC2 - PC3          | 330 Ohm, 10k Ohm and 330k Ohm, P-channel switches (high is off) |
| Measuring 1      | PD2 (A3)                 |      |
| Measuring 2      | PC4 (A2)                 |      |

Services:

- Mandatory
- RSBus Bootloader, see [[Services/Bootloader]]
- [[Services/Subscriptions|Subscriptions]], provide only

Modules:

- Resistive measurement, 2 instances
- Button
- LED

Page size: 64 bytes (fast mode)
Table size: 1 page
Storage: 512 B
### Valu v2.0 (CH32V203G8R6), Legacy Standalone Node with USB
| Feature           | Pins                                         | Note |
| ----------------- | -------------------------------------------- | ---- |
| LED-Button        | PA2                                          | Red, active high |
| Buttons           | PB15, PB14, PB13                             | Requires a pull-down; active high |
| Fan PWM output    | PA8                                          |      |
| Measuring         | PA6 (ADC6), PA1 (ADC1), PA0 (ADC0)           | Reference resistor not defined |
| LED strip outputs | PA14, PB8                                    |      |
| OLED Display      | MOSI PA7, CLK PA5, CS PA4, DRST PA3, DDC PB0 |      |
| I2C bus           | SCL PB6, SDA PB7                             |      |
| UART              | RX PA10, TX PA9                              |      |

Services:

- Mandatory
- USB Bootloader, see [[Services/Bootloader]]
- App interface over USB, see [[Services/App Interface]]
- Dynamic memory, under [[Services/Register]]
- [[Services/Script|Script]]

Modules:

- LED Display, 2 instances
- Fan Output, 1 instance
- LED-Button
- Button, 3 instances
- OLED Display (TODO)
- Resistive measurement, 3 instances

Page size: 256 bytes (fast mode)
Table size: 2 pages
Storage: 8 kB
