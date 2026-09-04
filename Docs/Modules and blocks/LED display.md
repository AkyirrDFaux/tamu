A generic LED display setting interface block, allows rendering shapes with textures and overlay effects. LED's are adressable, connected in series, layout or at least size must be specified.
#### Static Settings Block
Contains overall settings.

| Name                    | F.K:SP | Flags | Size              | Note                                                      |
| ----------------------- | ------ | ----- | ----------------- | --------------------------------------------------------- |
| Brightness              | 0      |       | Number            | %                                                         |
| Offset                  | 1      | P     | Matrix 3x3        | Defines the 0,0 screen position and default rotation      |
| Render KeyedBlock Index | 2      | P     | uint32            | Index of the block containing shapes textures and effects |
| Layout File Name        | 3      | TR,P  | Filename (8 char) | File containing the layout                                |
| Refresh Rate            | 4      | RO    | Number            | in FPS (averaged)                                         |
#### Layout file:

| Display width | Display Height | Table of LED indexes |
| ------------- | -------------- | -------------------- |
| uint8         | uint8          | W x H x uint16       |
Index 0xFFFF means missing LED, 0 is first led in chain, 1 is second and so on.
Table is row first.

#### Render Dynamic Block
This block contains geometry definitions with their selected textures and applied effects to be rendered on the display in the given order.
For each element to be processed (consecutively) a dictionary (Geometry/Texture/Effect) is added.
The dictionaries contain the descriptions via the keys.

TODO: Keys.