RS485 Bus with CSMA/CD, 115200 baud
Supply voltage for device 3V3 or directly from USB PD (9-20V)

The transmission can start if the line has been silent for the time-equivalent of 8 bytes + random small delay (0-7 bytes).
The sent data is immediately verified while sending (a feedback loop).
If a collision happens, stop the transmission and wait for silence with a new random delay.

Before the start of the transmission a start/sync byte is sent.

#### Router structure
Each router has it's main bus and branch busses.
It will not pass any messages except broadcast, until it has ID assigned.
It may be assigned only an ID from the main bus, filters ID assignments from branch busses (other cores), prevents discovery packets from being routed outside (main to branch).

### TODO:
Solve assignment conflicts on busses where two routers/core nets meet.

... ask if there is a router on the branch 
... respond if the router is facing out (branch) or in (main)
if facing out, ask if the core is the same...
... if yes, warn the user of a loop
... if no, the cores have to negotiate for the devices, based on if someone already knows them

