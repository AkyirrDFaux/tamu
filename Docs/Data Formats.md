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
- Enum - Generic enum, used in dictionaries
### Generic packet

| CRC8 | Flags | FragID | Payload Len | ID TGT | ID SRC | SRV TGT | SRV SRC | Payload |
| ---- | ----- | ------ | ----------- | ------ | ------ | ------- | ------- | ------- |
| 1    | 1     | 1      | 1           | 2      | 2      | 2       | 2       | 0-255   |
- CRC8 (covers everything after)
- Flags : REQACK (request response), START (first), STOP (last), TYPE (Request/Response)
- FragID : Sequential number, 0 default
### Common structs
- BlockIndex (uint8 x4) - Block, Field/Dictionary, Key, (Padding)
	Unspecified is 0xFF
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
	- ...