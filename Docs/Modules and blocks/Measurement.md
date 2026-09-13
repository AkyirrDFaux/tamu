### Resistive measurement
Voltage divider based measurement, reference resistor switching supported

| Name               | F.K:SP | Flags  | Size   | Note                                                                                 |
| ------------------ | ------ | ------ | ------ | ------------------------------------------------------------------------------------ |
| Sampling Rate      | 0      | P,(TR) | Number | Hz, Trigger is device implementation specific                                        |
| Sensor Type        | 1      | P      | Enum   |                                                                                      |
| Filter Coefficient | 2      | P      | Number | EMA coefficient (0-1), applies on raw ADC value.                                     |
| Deadzone           | 3      | P      | Number | > 0. The current output value is the center. 0 = off. Applies after unit conversion. |
| Measured Value     | 4      | RO     | Number | -/V/kOhm/Lux/°C                                                                      |
| Current Range      | 6      | RO     | Number | kOhm                                                                                 |
Sensor types: Raw Measurement, Raw Voltage, Raw Resistance, LDR 10K, NTC10K, NTC100K
### Acc&Gyr
An accelerometer + gyroscope combo.

| Name                  | F.K:SP | Flags | Size    | Note                                                              |
| --------------------- | ------ | ----- | ------- | ----------------------------------------------------------------- |
| Sampling Rate         | 0      | TR,P  | Enum    | Hz                                                                |
| Range Acceleration    | 1      | TR,P  | Enum    | m/s^2                                                             |
| Range Angular         | 2      | TR,P  | Enum    | rad/s                                                             |
| Acceleration Filter   | 3      | P     | Number  | EMA (0-1)                                                         |
| Angular Filter        | 4      | P     | Number  | EMA (0-1)                                                         |
| Deadzone acceleration | 5      | P     | Number  | > 0. The current output value is the center. Uses 2-norm. 0 = off |
| Deadzone angular      | 6      | P     | Number  | > 0. The current output value is the center. Uses 2-norm. 0 = off |
| Acceleration          | 7      | RO    | Vector3 | m/s^2                                                             |
| Angular Velocity      | 8      | RO    | Vector3 | rad/s                                                             |

