RS485 Bus with CSMA/CD, 115200 baud
Supply voltage for device 3V3 or directly from USB PD (9-20V)

The transmission can start if the line has been silent for the time-equivalent of 8 bytes + random small delay (0-7 bytes).
The sent data is immediately verified while sending (a feedback loop).
If a collision happens, stop the transmission and wait for silence with a new random delay.

Before the start of the transmission a start/sync byte is sent.