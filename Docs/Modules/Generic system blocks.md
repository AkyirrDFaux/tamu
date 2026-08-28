#### Fan output
Simple PWM output on the specified pin.

| Function  | DataType | Direction | Callback |
| --------- | -------- | --------- | -------- |
| Frequency | uint32   | In        | Yes      |
| Duty      | Number   | In        | Yes      |
#### Acc&Gyr

| Function        | DataType | Direction | Callback | Unit                       |
| --------------- | -------- | --------- | -------- | -------------------------- |
| Sampling Rate   | Number   | In        | Yes      | Hz                         |
| Acceleration    | Vector3  | Out       | -        | m/s^2                      |
| AngularVelocity | Vector3  | Out       | -        | rad/s                      |
| AccFilter       | Number   | In        | No       | Number of averaged samples |
| AngFilter       | Number   | In        | No       | Number of averaged samples |
#### LED-Button
Button and LED on one pin. The LED is controlled by the LEDState field; the button is
reported through the Button field (while the LED is off - lighting the LED disables
button reading).

Hardware (Tamu v2.0A, pin 2): LED and button are **in series** with the pin connected
in the middle - button to ground, LED (with series resistor) to VCC, plus an external
pull-up resistor from the pin to VCC. So the line idles HIGH (pull-up): driving the pin
LOW lights the LED (active-LOW), and reading the pin LOW means the button is pressed
(active-LOW). The LED must be off to read the button (while driven, the line cannot be
sampled).

| Function | DataType | Direction | Callback |
| -------- | -------- | --------- | -------- |
| LEDState | bool     | In        | Yes      |
| Button   | bool     | Out       | -        |

#### Resistive measurement
Voltage divider based measurement, reference resistor switching supported

| Function           | DataType | Direction | Callback         | Unit                       |
| ------------------ | -------- | --------- | ---------------- | -------------------------- |
| Sampling Rate      | Number   | In        | Device dependent | Hz                         |
| Filter Coefficient | Number   | In        | No               | Number of averaged samples |
| Sensor Type        | Enum     | In        | No               |                            |
| Measured Value     | Number   | Out       | -                | bit/V/kOhm/Lux/°C          |
| Current Range      | Number   | Out       | -                | kOhm                       |
Sensor types : Raw Measurement, Raw Voltage, Raw Resistance, LDR 10K, NTC10K