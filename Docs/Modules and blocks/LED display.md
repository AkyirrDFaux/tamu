A generic LED display interface, allows rendering shapes with textures and overlay effects. LED's are adressable, connected in series, layout or at least size must be specified.
#### Static Settings Block
Contains overall settings.

| Name                    | F.K:SP | Flags | Size              | Note                                                                 |
| ----------------------- | ------ | ----- | ----------------- | -------------------------------------------------------------------- |
| Brightness              | 0      |       | Number            | %                                                                    |
| Offset                  | 1      | P     | Matrix 2x3        | Transformation, Defines the 0,0 screen position and default rotation |
| Render KeyedBlock Index | 2      | P     | uint32            | Index of the block containing shapes textures and effects            |
| Layout File Name        | 3      | TR,P  | Filename (8 char) | File containing the layout                                           |
| Refresh Rate            | 4      | RO    | Number            | in FPS (averaged)                                                    |
#### Layout file

| Display width | Display Height | Table of LED indexes |
| ------------- | -------------- | -------------------- |
| uint8         | uint8          | W x H x uint16       |
Index 0xFFFF means missing LED, 0 is first led in chain, 1 is second and so on.
Table is row first.

#### Rendering Dynamic Block
Rendering pipeline is based on two parts, mask and fill.
All the parts are processed consecutively in the (index) order, that they are in the block.

The mask is an alpha channel modified by geometric definitions.
Geometry is defined using a dictionary.

| Key name          | Key | Usual type       | Note                              |
| ----------------- | --- | ---------------- | --------------------------------- |
| Operation type    | 0   | Enum             | Replace, Add, Cut, Intersect, XOR |
| Geometry shape    | 1   | Enum             |                                   |
| Position          | 2   | Matrix 2x3       | 2D transformation                 |
| Size              | 3   | Vector<2>/Number |                                   |
| Fade              | 4   | Number           | in pixels                         |
| Alpha             | 5   | Number           | default 1, 0 to 1 range           |
| Rounding          | 6   | Number           | in pixels                         |
| Angles            | 7   | Number/Vector    |                                   |
| Point number      | 8   | Integer          |                                   |
| Point coordinates | 9   | Matrix 2xN       |                                   |
| Noise seed        | 10  | Integer          |                                   |
Not all shapes have to interact with every parameter.

Shape list:
- Fill
- HalfFill
- Square
- Rectangle
- Trapezoid
- Circle
- Ellipse
- DoubleParabola
- Triangle (equilateral) = size only 
- Triangle (isosceles) = angle + side length
- Polygon
- Star
- Mesh
- Noise

The fill is the texture or effect applied in the area specified by the mask.
Textures and effects are described using a dictionary.

| Key name            | Key | Usual type       | Note                              |
| ------------------- | --- | ---------------- | --------------------------------- |
| Texture/Effect type | 0   | Enum             |                                   |
| Position            | 1   | Matrix 2x3       | 2D transformation, defines center |
| Size                | 2   | Vector<2>/Number |                                   |
| Colour 1            | 3   | Colour (RGBA)    |                                   |
| Colour 2            | 4   | Colour (RGBA)    |                                   |
| Colour 3            | 5   | Colour (RGBA)    |                                   |
| Amount              | 6   | Number           |                                   |
Not all shapes have to interact with every parameter.
Texture list:
- Fill
- Gradient linear
- Gradient circular
- Bitmap (TODO)
Effect list (continues texture enum):
- Colour inversion
- Hue shift
- Contrast change
- Brightness change