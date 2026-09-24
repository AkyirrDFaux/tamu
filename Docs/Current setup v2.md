2x DAS
- Temperature sensor
- LDR
2x LED display
- Light Mode:
	- White background
	- Green circular iris, slight horizontal fade
	- Black vertical pupil (double parabola)
	- Black lid (closes from top)
- Dark mode:
	- Black background
	- A dark green circular iris, slight horizontal fade
	- Desaturated green vertical pupil (double parabola)
	- Black lid (closes from top)
1x Fan

Dynamic block 0 : Subscriptions
Dynamic block 1 : Left Eye
Dynamic block 2 : Right Eye

Script 1 : Temperature regulation
- Set PWM duty based on temperature 
- Input 0 : Target temperature
- Input 1 : P constant
Script 2 : Eye movement
- Rotational axis of gyroscope (XY) moves circle and pupil position
- Input 0: Offset (Vector2), X is flipped for one of the eyes.
- Input 1: Sensitivity (Matrix 2x3), multiplier for movement (not a transformation), XYZ (gyro) ->XY (display).
Script 3 : Lid timer
-  Input 0 : Delay between blinks, default 10s
-  Input 1 : Movement time (each way), default 200ms 
Script 4 : Brightness regulation
- Uses one LDR to set brightness on each display, switches between dark mode and light mode independently.
- Input 0 : Switch between auto and manual, default auto
- Input 1 : Manual mode Left eye switch selection (light/dark)
- Input 2 : Manual mode Right eye switch selection (light/dark)