### Button
Simple button.

| Name             | F.K:SP | Flags | Size  | Note                         |
| ---------------- | ------ | ----- | ----- | ---------------------------- |
| Button raw state | 0      | RO    | bool  | true = pressed, false = free |
| Edge detection   | 1      | P     | enum  | None/Rising/Falling/Both     |
| Edge counter     | 2      | RO    | uint8 | Resets on overrun            |
### LED
Simple LED.

| Name     | F.K:SP | Flags | Size | Note                   |
| -------- | ------ | ----- | ---- | ---------------------- |
| LEDState | 0      | TR    | bool | true = on, false = off |
### LED-Button
Button and LED on one pin. The LED is controlled by the LEDState field; the button is
reported through the Button field (if LED is on button reading is disabled).

| Name             | F.K:SP | Flags | Size  | Note                         |
| ---------------- | ------ | ----- | ----- | ---------------------------- |
| Button raw state | 0      | RO    | bool  | true = pressed, false = free |
| Edge detection   | 1      | P     | enum  | None/Rising/Falling/Both     |
| Edge counter     | 2      | RO    | uint8 | Resets on overrun            |
| LEDState         | 3      | TR    | bool  | true = on, false = off       |

