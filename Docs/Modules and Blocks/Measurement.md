The measurement blocks: a resistive measurement, and a combined accelerometer and gyroscope.
### Resistive Measurement
A voltage divider based measurement, with reference resistor switching.

| Name               | F.K | Flags  | Size   | Note |
| ------------------ | --- | ------ | ------ | ---- |
| Sampling Rate      | 0   | P, (TR) | Number | Hz. The trigger is device-implementation specific: it clamps the value so the stored one equals the applied one. |
| Sensor Type        | 1   | P      | `Enum` |      |
| Filter Coefficient | 2   | P, (TR)      | Number | EMA coefficient (0-1), applied to the raw ADC value |
| Measured Value     | 3   | RO     | Number | -/V/kOhm/Lux/°C |
| Current Range      | 4   | RO     | Number | kOhm |

Sensor types: Raw Measurement, Raw Voltage, Raw Resistance, LDR 10K, NTC10K, NTC100K
### Acc & Gyr
A combined accelerometer and gyroscope.

| Name                | F.K | Flags | Size       | Note |
| ------------------- | --- | ----- | ---------- | ---- |
| Sampling Rate       | 0   | TR, P | `Enum`     | Hz |
| Range Acceleration  | 1   | TR, P | `Enum`     | m/s^2 |
| Range Angular       | 2   | TR, P | `Enum`     | rad/s |
| Acceleration Filter | 3   | P     | Number     | EMA (0-1) |
| Angular Filter      | 4   | P     | Number     | EMA (0-1) |
| Acceleration        | 5   | RO    | `Vector<3>` | m/s^2 |
| Angular Velocity    | 6   | RO    | `Vector<3>` | rad/s |
