### Basic types
- None (No value there, a placeholder/spacer metadata or a deleted entry)
- Undefined (Don't know yet, display in hex format if there is anything)
- Serial Number - 14 byte UUID
- ID - 16bit (4 bit net + 12 bit device) 
	- 0 = Invalid/Unassigned
	- 0xFFFFFFFF = Broadcast
	- Net 0 = local
- Bool - true/false
- Index  - 32bit signed integer
- Number - 16.16 signed fixed point
- Vector - Size flexible (Template) 1D array of numbers
- Matrix - Size flexible (Template) 2D array of numbers, first 32 bits is size (uint16 height + width)
- Colour - 4xUint8, RGBA
- String - Text (8bit per character, standard)
- BlockIndex (uint8 x4) - Block, Field/Dictionary, Key, (Padding)
	Unspecified is 0xFF
- Enum - Generic enum, used in dictionaries
### Generic packet

| Section | Field          | Size                 | Note                                                                                                   |
| ------- | -------------- | -------------------- | ------------------------------------------------------------------------------------------------------ |
| Generic | CRC8           | uint8                | covers everything after                                                                                |
|         | Flags          | 8 bits               |                                                                                                        |
|         | Priority       | uint8                | 0 = highest, default 128                                                                               |
|         | Payload Length | uint8                | in multiples of 4 bytes (for 32bit alignment), max 5+64, includes payload information                  |
| ID      | TGT            | uint16               |                                                                                                        |
|         | SRC            | uint16               |                                                                                                        |
| SRC     | TGT            | uint16               |                                                                                                        |
|         | SRC            | uint16               |                                                                                                        |
| Payload | Information    | max 5 * 4 = 20 bytes | Flexible size, can be ommited, service specific. Example: fragmentation, indexing, metadata, filename. |
|         | Actual payload | max 256 bytes        | Flexible size, service specific.                                                                       |
- Flags : REQACK (request response), START (first), STOP (last), TYPE (Request/Response), FRAG (first 4 payload bytes are fragmentation information, uint16 current frag. segment + uint16 total segments)
- Priorities: Errors (highest) -> TimeSync packets -> Other  -> Streams -> Logs (lowest)
Maximum length 288 bytes total, all devices have to handle it in full.
### Common structs
- BlockMeta (6bit, 10bit , 8bit x2) - Flags, Type, (Padding OR Key), Length of Value
	Flags:
	- ReadOnly - RAM only (non-writable through network, only by direct function, never save)
	- NotSaved - RAM only (there has *likely* a change been made)
	- ScriptUpdated - RAM only (Save only if user requests this specific entry)
	- Valid - Flash only (if 0, there is a newer version, ignore this one)
- SRV - 16bit (8bit service type + 8bit custom identifier)
- Capability (32bit-field)
	- Core
	- Router
	- CLI
	- Dynamic Memory
	- Keyed Memory
	- Scripts
	- Bootloader
	- ...