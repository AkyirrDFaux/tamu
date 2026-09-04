#### Fan output
Simple PWM output on the specified pin.

| Name      | F.K:SP | Flags | Size   | Note    |
| --------- | ------ | ----- | ------ | ------- |
| Frequency | 0      | TR, P | uint32 | Hz      |
| Duty      | 1      | TR    | uint32 | 0-100 % |
#### Acc&Gyr
An accelerometer + gyroscope combo.

| Name            | F.K:SP | Flags | Size    | Note                              |
| --------------- | ------ | ----- | ------- | --------------------------------- |
| Sampling Rate   | 0      | TR,P  | Number  | Hz                                |
| Acceleration    | 1      | RO    | Vector3 | m/s^2                             |
| AngularVelocity | 2      | RO    | Vector3 | rad/s                             |
| AccFilter       | 3      | P     | Number  | Number of averaged samples (>= 1) |
| AngFilter       | 4      | P     | Number  | Number of averaged samples (>= 1) |
#### LED-Button
Button and LED on one pin. The LED is controlled by the LEDState field; the button is
reported through the Button field (if LED is on button reading is disabled).

| Name     | F.K:SP | Flags | Size | Note |
| -------- | ------ | ----- | ---- | ---- |
| LEDState | 0      | TR    | bool |      |
| Button   | 1      | RO    | bool |      |
#### Button
Simple button.

| Name   | F.K:SP | Flags | Size | Note |
| ------ | ------ | ----- | ---- | ---- |
| Button | 0      | RO    | bool |      |
#### Resistive measurement
Voltage divider based measurement, reference resistor switching supported

| Name               | F.K:SP | Flags  | Size   | Note                                          |
| ------------------ | ------ | ------ | ------ | --------------------------------------------- |
| Sampling Rate      | 0      | P,(TR) | Number | Hz, Trigger is device implementation specific |
| Filter Coefficient | 1      | P      | Number | Number of averaged samples (>= 1)             |
| Sensor Type        | 2      | P      | Enum   |                                               |
| Measured Value     | 3      | RO     | Number | bit/V/kOhm/Lux/°C                             |
| Current Range      | 4      | RO     | Number | kOhm                                          |
Sensor types: Raw Measurement, Raw Voltage, Raw Resistance, LDR 10K, NTC10K