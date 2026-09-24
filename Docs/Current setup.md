2x DAS
- Temperature sensor
- LDR
2x LED display
- White background
- Green circle, slight horizontal fade
- Black vertical pupil (double parabola)
- Black lid (closes from top)
1x Fan

Dynamic block 0 : Subscriptions
Dynamic block 1 : Left Eye
Dynamic block 2 : Right Eye

Script 1 : Temperature regulation
- Set PWM duty based on temperature 
Script 2 : Eye movement
- Rotational axis of gyroscope (XY) calculates circle and pupil position
Script 3 : Lid timer
-  10s delay between blinks
-  200ms movement (each way)
Script 4 : Brightness regulation
- Uses one LDR to set brightness on each display