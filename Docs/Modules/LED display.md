A generic LED display setting interface block, allows rendering shapes with textures and overlay effects.
#### System Block:
Contains overall settings.

| Function                | DataType           | Direction | Callback | Note                                                      |
| ----------------------- | ------------------ | --------- | -------- | --------------------------------------------------------- |
| Brightness              | Number             | In        | No       | %                                                         |
| Offset                  | Matrix 3x3         | In        | No       | Defines the 0,0 screen position and default rotation      |
| Render KeyedBlock Index | uint32             | In        | No       | Index of the block containing shapes textures and effects |
| Layout File Name        | Name (8char/6byte) | In        | Yes      | File containing the layout                                |
| Refresh Rate            | Number             | Out       | -        | in FPS averaged                                           |
#### Layout file:

| Display width | Display Height | Table of LED indexes |
| ------------- | -------------- | -------------------- |
| uint8         | uint8          | W x H uint16         |
Index FFFF means missing LED, 0 is first led in chain, 1 is second and so on.
Table is row first.

#### Render Keyed Block:
This block contains geometry definitions with their selected textures and effect application to be rendered on the display in the given order.

Geometries can be combined based on the operation, texture always clears the buffer and applies the texture in the given areas.


| Index       | BlockType      |
| ----------- | -------------- |
| 0           | Keyed Geometry |
| 0:Type      | Enum           |
| 0:Operation | Add            |
| 0:Height    | Number         |
| 0:Width     | Number         |
| ...         |                |
| 1           | Enum Geometry  |
| ...         |                |
| 2           | Keyed Texture  |
| 2:Type      | Enum           |
| 2:Colour    | Colour         |
| ...         |                |
| 3           | Keyed Effect   |
| 3: Type     | Enum           |
| ...         |                |
