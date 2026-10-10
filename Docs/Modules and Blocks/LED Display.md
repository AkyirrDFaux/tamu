A generic LED display interface that renders shapes with textures and overlay effects. The LEDs are addressable and connected in series; the layout, or at least the size, must be specified.
### Static Settings Block
Contains the overall settings.

| Name                    | F.K | Flags | Size      | Note |
| ----------------------- | --- | ----- | --------- | ---- |
| Brightness              | 0   |       | Number    | 0-100 %; mapped linearly onto the layout's brightness limit |
| Offset                  | 1   | P     | `Matrix<2,3>` | A transformation that defines the 0,0 screen position and the default rotation |
| Render Block Index | 2   | P     | `Index`   | Index of the block containing shapes, textures and effects; -1 renders nothing |
| Layout File Name        | 3   | TR, P | `Filename` | The file containing the layout |
| Refresh Rate            | 4   | RO    | Number    | In FPS, averaged |
### Layout File
| Name                  | Size           | Note |
| --------------------- | -------------- | ---- |
| Brightness limit      | uint8          | 0-255, read as a percentage: the physical ceiling the 0-100 % Brightness setting maps onto (178 = 70 %) |
| Display width         | uint8          |      |
| Display height        | uint8          |      |
| Table of LED indexes  | uint16[W x H]  | Row first |

An index of `0xFFFF` means a missing LED. Index 0 is the first LED in the chain, 1 the second, and so on.
### Rendering Dynamic Block
The rendering pipeline has two parts, mask and fill. All parts are processed consecutively, in the index order they have in the block.

The mask is an alpha channel modified by geometric definitions, and the geometry is defined using a dictionary.

| Key Name          | Key | Usual Type        | Note |
| ----------------- | --- | ----------------- | ---- |
| Geometry shape    | 1   | `Enum`            |      |
| Operation type    | 2   | `Enum`            | Replace, Add, Cut, Intersect, XOR |
| Position          | 3   | `Matrix<2,3>`     | 2D transformation; the writer stores the translation pre-rotated by the linear part (`t' = L * t`) so a shape's centre stays at `-t` under rotation |
| Size              | 4   | `Vector<2>` or `Number` |      |
| Fade              | 5   | `Number`          | In pixels |
| Alpha             | 6   | `Number`          | Default 1, range 0 to 1 |
| Rounding          | 7   | `Number`          | In pixels |
| Angles            | 8   | `Number`          |      |
| Point number      | 9   | `int32`           |      |
| Point coordinates | 10  | `Matrix<2,N>`     | TODO |
| Noise seed        | 11  | `int32`           |      |

Not every shape interacts with every parameter.

Shape list:

- Fill
- HalfFill
- Square
- Rectangle
- Trapezoid
- Circle
- Ellipse
- DoubleParabola
- Triangle: isosceles (angle and side length)
- Polygon
- Star
- Mesh (TODO)
- Noise

The fill is the texture or effect applied in the area specified by the mask. Textures and effects are described using a dictionary.

| Key Name            | Key | Usual Type        | Note |
| ------------------- | --- | ----------------- | ---- |
| Texture/Effect type | 1   | `Enum`            |      |
| Position            | 2   | `Matrix<2,3>`     | A 2D transformation that defines the centre; pre-rotated by the linear part (`t' = L * t`) like the geometry Position |
| Size                | 3   | `Vector<2>` or `Number` |      |
| Colour 1            | 4   | `Colour`          |      |
| Colour 2            | 5   | `Colour`          |      |
| Colour 3            | 6   | `Colour`          |      |
| Amount              | 7   | `Number`          |      |

Not every texture interacts with every parameter.

Texture list:

- Fill
- Gradient linear
- Gradient circular

The effect list continues the texture enum:

- Colour inversion
- Hue shift
- Contrast change
- Brightness change
- Bitmap (reserved)
