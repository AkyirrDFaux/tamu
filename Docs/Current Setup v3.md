The evaluation setup the firmware is exercised against: two DAS nodes, two LED displays and a fan, with five scripts running on the core.
### Hardware
- 2x DAS
	- Temperature sensor
	- LDR
- 2x [[Modules and Blocks/LED Display|LED display]]
	- Light mode:
		- White background
		- Green circular iris, slight horizontal fade
		- Black vertical pupil (double parabola by default)
		- Black lid, closing from the top
	- Dark mode:
		- Black background
		- A dark green circular iris, slight horizontal fade
		- Desaturated green vertical pupil (double parabola by default)
		- Black lid, closing from the top
- 1x Fan, see [[Modules and Blocks/Generic System Blocks|Fan Output]] (not connected in this setup)
### Dynamic Blocks
- Dynamic block 0: [[Services/Subscriptions|Subscriptions]]
- Dynamic block 1: Left Eye
- Dynamic block 2: Right Eye
### Scripts
Script 1: Temperature regulation

- Sets the PWM duty from the temperature
- Input 0: Target temperature
- Input 1: P constant

Script 2: Eye movement

- The rotational axis of the gyroscope (XY) moves the circle and pupil position
- Input 0: Offset (`Vector<2>`). X is flipped for one of the eyes.
- Input 1: Sensitivity (`Matrix<2,3>`), a multiplier for the movement and not a transformation; XYZ from the gyro to XY on the display.
- Output 0: offset L
- Output 1: offset R

Script 3: Lid timer

- Input 0: Blink delay, default 10 s
- Input 1: Movement time, default 200 ms
- Input 2: Force close
- Input 3: Max opening

Script 4: Brightness regulation

- Uses one LDR to set the brightness of each display, switching between dark and light mode independently.

| <10 Lux | 100 Lux | 3000 Lux | >8k Lux |
| ------- | ------- | -------- | ------- |
| 7%      | 14%     | 60%      | 100%    |

- Input 0: Manual mode, default auto
- Input 1: Manual mode, left eye switch selection (light or dark)
- Input 2: Manual mode, right eye switch selection (light or dark)

Script 5: Emote selector

- Replaces the pupil based on the selected emote, in both light and dark mode. Forces a blink when the pupil shape changes.
- Emotes:
	- Normal (double parabola)
	- Happy (caret)
	- Dead (cross)
	- Annoyed (double parabola, with a slightly closed lid)
- Input 0: Selected emote (a custom enum, internally an integer)
