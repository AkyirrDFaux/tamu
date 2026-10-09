The data types used across the protocol, with their enum value and encoding. A type needing more than a line is described in the chapters below the table.

| Name           | Enum  | Description                                                              |
| -------------- | ----- | ------------------------------------------------------------------------ |
| `None`         | 0x00  | No value there. A placeholder or spacer in metadata, or a deleted entry. |
| `Undefined`    | 0x01  | Not known yet; displayed in hex when there is anything to show.          |
| `SN`           | 0x02  | A serial number, a 14 byte UUID.                                         |
| `Id`           | 0x03  | A device ID, 16 bits: 6 of net and 10 of device. Described below.        |
| `Bool`         | 0x04  | true or false.                                                           |
| `Index`        | 0x05  | A 32bit signed integer.                                                  |
| `Number`       | 0x06  | A signed 16.16 fixed point value.                                        |
| `Vector`       | 0x07  | A 1D array of `Number`, templated as `Vector<N>`.                        |
| `Matrix`       | 0x08  | A 2D array of `Number`, templated as `Matrix<N,M>`.                      |
| `Colour`       | 0x09  | Four `uint8` values, RGBA.                                               |
| `String`       | 0x0A  | Text, 8 bits per character, standard.                                    |
| `Filename`     | 0x0B  | 8 characters.                                                            |
| `Name`         | -     | 16 characters. The reusable type for any 16-character name.              |
| `Enum`         | 0x0C  | A generic enum, used in dictionaries.                                    |
| `Deleted`      | 0x0D  | A deleted entry.                                                         |
| `Uint32`       | 0x0E  | An unsigned 32bit integer.                                               |
| `DevType`      | 0x0F  | A device type, as listed in [[Devices]].                                 |
| `BlockInfo`    | 0x10  | A struct for pointing scripts.                                           |
| `UnknownKeyed` | 0x100 | The generic dictionary marker.                                           |
| `Geometry`     | 0x101 | A dictionary describing a geometric shape.                               |
| `Texture`      | 0x102 | A dictionary describing a texture.                                       |
| `Effect`       | -     | A dictionary describing a graphical effect.                              |
### ID
A device ID is 16 bits, of which 6 bits are the net and 10 bits are the device.

- Net
	- 0 is net local
	- 0x3F is a broadcast into all nets
	- 62 (64-2) valid nets
- Device
	- 0 is unassigned
	- 0x3FF is a broadcast
	- 0x3FE is a branch broadcast, not routed away
	- 1021 (1024-3) maximum valid devices
	- 1 is always the core

An address is written as `NetID.Device`:

- 3F.0: all unassigned devices
- 3F.1: all cores
- 3F.3FF: everything
- 0.0: local unassigned devices
- 0.1: local core
- 0.8: local device 8
- 0.3FF: local broadcast
- 0.3FE: branch broadcast
- 3.2: device 2 in net 3
### Dictionaries
The keyed types, `UnknownKeyed`, `Geometry`, `Texture` and `Effect`, describe structured data as a dictionary of keys. The generic dictionary is always key 0 with no value, a length of 0 and an invalid offset, which makes it a marker. The keys of the geometry and the texture dictionaries are listed in [[Modules and Blocks/LED Display]].
