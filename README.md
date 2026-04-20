# Tamu
Firmware / Micro-OS for 32-bit microcontroller boards
Complementary app: https://github.com/AkyirrDFaux/tamuapp

# Operation Manual

If any references are under output {0} node, the value will be copied there.

**Set** - Copies a source value to the output destination.\
Any | (Reference)

**Delete** - Deletes the values or objects at the specified locations.\
None | Reference [N]

**ToBool** - Evaluates the input and converts it to a Boolean.\
Bool | Scalar

**ToByte** - Converts the input to an 8-bit unsigned integer (0-255).\
Byte | Scalar

**ToInt** - Converts the input to a 32-bit signed integer.\
Integer | Scalar

**ToNumber** - Converts the input to a fixed-point Number.\
Number | Scalar

**ToVector2D** - Composes two values into a 2D Vector.\
Vector2D | Scalar [2]

**ToVector3D** - Composes three values into a 3D Vector.\
Vector3D | Scalar [3]

**ToCoord2D** - Composes two values into a 2D Screen Coordinate.\
Coord2D | Vector2D, Number

**ToColour** - Composes four bytes into an RGBA color.\
Colour | Byte [3/4]

**ToColourHSV** - Converts Hue, Saturation, and Value into an RGBA color.\
Colour | Byte [3/4]

**Extract** - Pulls a specific primitive part from a packed type by index.\
Number | Vector2D, 0-1\
Number | Vector3D, 0-2\
Vector2D | Coord2D, 0-1\
Byte | Colour, 0-3

**IsEqual** - Returns true if two values are identical.\
Bool | Scalar, Scalar

**IsGreater** - Returns true if the first value is greater than the second.\
Bool | Scalar, Scalar

**IsSmaller** - Returns true if the first value is smaller than the second.\
Bool | Scalar, Scalar

**Add** - Calculates the sum of two values or vectors.\
Scalar | Scalar [N]\
Vector2D | Vector2D [N]\
Vector3D | Vector3D [N]

**Subtract** - Calculates the difference between two values or vectors.\
Scalar | Scalar [N]\
Vector2D | Vector2D [N]\
Vector3D | Vector3D [N]

**Multiply** - Calculates the product of two values.\
Scalar | Scalar [N]\
Vector2D | Vector2D [N]\
Vector3D | Vector3D [N]

**Divide** - Calculates the quotient of two values.\
Scalar | Scalar [N]\
Vector2D | Vector2D [N]\
Vector3D | Vector3D [N]

**Power** - Raises the base to the specified exponent.\
-Missing-

**Absolute** - Returns the non-negative magnitude of a value.\
-Missing-

**Min** - Returns the smaller of two values.\
Scalar | Scalar [N]\

**Max** - Returns the larger of two values.\
Scalar | Scalar [N]\

**Modulo** - Returns the remainder of a division.\
Scalar | Scalar [N]\

**Random** - Generates a random number within a range.\
Number | Number (Min), Number (Max)

**Sine** - Generates a sine wave value based on phase.\
Number | Scalar (Input), Scalar (Multiplier), Scalar (Phase)

**Square** - Generates a square wave value based on phase.\
Number | Scalar (Input), Scalar (Multiplier), Scalar (Phase)

**Sawtooth** - Generates a sawtooth wave value based on phase.\
Number | Scalar (Input), Scalar (Multiplier), Scalar (Phase)

**Magnitude** - Returns the scalar length of a vector.\
Number | Vector2D\
Number | Vector3D

**Angle** - Returns the polar angle of a 2D vector.\
Number | Vector2D

**Normalize** - Resizes a vector to a magnitude of 1.0.\
Vector2D | Vector2D\
Vector3D | Vector3D

**DotProduct** - Calculates the scalar product of two vectors.\
Vector2D | Vector2D [2]\
Vector3D | Vector3D [2]

**CrossProduct** - Calculates the cross product of two 3D vectors.\
Vector3D | Vector3D [2]

**Clamp** - Constrains a value between a minimum and maximum.\
Scalar | Scalar, Scalar (Min), Scalar (Max)

**Deadzone** - Returns zero if the value is within a small range of zero.\
Scalar | Scalar, Scalar (Size)

**LinInterpolation** - Linearly blends between two values.\
Scalar | Scalar (A), Scalar (B), Scalar (Alpha), (Scalar (Alpha Scaling))

**And** - Returns true if all provided inputs are true.\
Bool | Bool [2]

**Or** - Returns true if any provided input is true.\
Bool | Bool [2]

**Not** - Inverts the state of a boolean.\
Bool | Bool

**Edge** - Returns true for one cycle on a False-to-True transition.\
Bool | Bool

**SetReset** - A latch where the first input sets and the second resets.\
Bool | Bool (Set), Bool (Reset)

**Delay** - Delays execution for X ms, outputs remaining or elapsed time.\
Integer (Elapsed/Remaining ms) | Integer (Period ms), Bool (Elapsed/Remaining mode), Integer (State)

**IfSwitch** - Executes branch N.\
Integer (Selector) | Operation [N]

**SetRunOnce** - Sets target objects to execute exactly once.\
Bool (State) | Reference [N]

**SetRunLoop** - Toggles persistent execution for target objects.\
Bool (State) | Reference [N]

**SetReference** - Sets a reference to target.\
Reference | None

**Save** - Triggers a non-volatile memory write for target objects.\
None | Reference [N]