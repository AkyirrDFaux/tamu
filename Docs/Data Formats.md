### Basic types
- None (0) - No value there, a placeholder/spacer metadata or a deleted entry
- Undefined (1) - Don't know yet, display in hex format if there is anything)
- Serial Number - 14 byte UUID
- ID - 16bit (6 bit net + 10 bit device) 
	- Net
		- 0 = Net Local
		- 0x3F = Broadcast into all nets
		- 62 (64-2) valid nets
	- Device
		- 0 = Unassigned
		- 0x3FF = Broadcast
		- 0x3FE = Branch Broadcast (Do not route away)
		- 1021 (1024 - 3) maximum valid devices
		- 1 is always core
	- Example:
		- 3F.0 = All unassigned devices
		- 3F.1 = All cores
		- 3F.3FF = Everything
		- 0.0 = Local Unassigned devices
		- 0.1 = Local Core
		- 0.8 = Local Device 8
		- 0.3FF = Local broadcast
		- 0.3FE = Branch Broadcast
		- 3.2 = Device 2 in net 3
- Bool - true/false
- Index  - 32bit signed integer
- Number - 16.16 signed fixed point
- Vector - Size flexible (Template) 1D array of numbers
- Matrix - Size flexible (Template) 2D array of numbers, first 32 bits is size (uint16 height + width)
- Colour - 4xUint8, RGBA
- String - Text (8bit per character, standard)
- Filename - 8 chars
- Enum - Generic enum, used in dictionaries
- Generic dictionary (0x0100) - Always Key 0, no value (length 0, offset invalid), works as a marker
- Geometry - Dictionary containing keys describing a geometric shape
- Texture - Dictionary containing keys describing a texture
- Effect - Dictionary containing keys describing a graphical effect
### Generic packet

| Section | Field          | Size          | Note                                                                                  |
| ------- | -------------- | ------------- | ------------------------------------------------------------------------------------- |
| Generic | CRC8           | uint8         | covers everything after                                                               |
|         | Flags          | 8 bits        |                                                                                       |
|         | Priority       | uint8         | 0 = highest, default 128                                                              |
|         | Payload Length | uint8         | in multiples of 4 bytes (for 32bit alignment), max 5+64, includes payload information |
| ID      | TGT            | uint16        |                                                                                       |
|         | SRC            | uint16        |                                                                                       |
| FN      | CMD            | uint16        | Command                                                                               |
|         | TRID           | uint16        | Transaction ID                                                                        |
| Payload |                | max 116 bytes | Flexible size, command specific.                                                      |
- Flags : REQACK (request response), START (first), STOP (last), TYPE (Request/Response), FRAG (first 4 payload bytes are fragmentation information, uint16 current frag. segment + uint16 total segments)
- Priorities: Errors (highest) -> TimeSync packets -> Other  -> Streams -> Logs (lowest)
Maximum length 128 bytes total, all devices have to handle it in full.