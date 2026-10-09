The simple input and output blocks: a button, an LED, and the combined LED-Button.
### Button
A simple button.

| Name             | F.K | Flags | Size | Note |
| ---------------- | --- | ----- | ---- | ---- |
| Button raw state | 0   | RO    | Bool | true is pressed, false is free. |
### LED
A simple LED.

| Name     | F.K | Flags | Size | Note |
| -------- | --- | ----- | ---- | ---- |
| LEDState | 0   | TR    | Bool | true is on, false is off. |
### LED-Button
A button and an LED on one pin. The LED is controlled by the LEDState field, and the button is reported through the Button field. Reading the button is disabled while the LED is on.

| Name             | F.K | Flags | Size | Note |
| ---------------- | --- | ----- | ---- | ---- |
| Button raw state | 0   | RO    | Bool | true is pressed, false is free. |
| LEDState         | 3   | TR    | Bool | true is on, false is off. |
