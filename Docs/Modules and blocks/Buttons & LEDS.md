### Button
Simple button.

| Name             | F.K:SP | Flags | Size  | Note                         |
| ---------------- | ------ | ----- | ----- | ---------------------------- |
| Button raw state | 0      | RO    | bool  | true = pressed, false = free |
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
| LEDState         | 3      | TR    | bool  | true = on, false = off       |

