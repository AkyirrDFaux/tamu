Use define `USE_SCRIPTS`.

Variables, constants and I/O are fixed in type and size. Variables live in a static RAM array inside the script. Constants work as read-only variables. I/O is a register, which is dynamic memory without persistence. Inputs need default values and a specification for the UI, such as whether they render as a switch or a button, and their names. Each script is stored as its own file, `SCR_XXX`.

Scripts are pre-loaded first, which creates their register and variable space. One table stores the currently loaded Script File IDs, another the allocated sizes of variable space memory, and a third the loaded script states. Variable space memory holds the instruction counter at its start.
#### File Blocks
| Name                    | Size                   | Note |
| ----------------------- | ---------------------- | ---- |
| Properties              | uint32                 |      |
| Input count             | uint8                  |      |
| Output count            | uint8                  |      |
| Variable count          | uint8                  |      |
| Constant count          | uint8                  |      |
| Constants length        | uint32                 |      |
| Input defaults length   | uint32                 |      |
| Instruction length      | uint32                 |      |
| UI info size            | uint32                 |      |
| Input types and size    | ValueInfo[N1]          |      |
| Output types and size   | ValueInfo[N2]          |      |
| Variable types and size | ValueInfo[N3]          |      |
| Constant types and size | ValueInfo[N4]          |      |
| Constant values         | Constants length       |      |
| Input defaults          | Input defaults length  |      |
| Instructions            | Instruction length     | Symbols |
| UI info                 | UI info length         |      |

Properties carry information about the script type, such as whether it loads on boot.

The UI info contains the function name; the input, output, variable and constant names; the input limits; and the UI type.

Predefines are commonly reused enums and small data, which compress better as symbols than as stored constants.
#### Symbol
A symbol is 32 bits: `Type` and `Subtype` are `uint8`, and `Value` is `uint16`.

| Type            | Subtype                             | Value |
| --------------- | ----------------------------------- | ----- |
| Instruction     | Subtype (Math/Service/Flow/Time...) | Instruction subtype enum |
| Input variable  | -                                   | Input Index |
| Output variable | -                                   | Output Index |
| Variable        | -                                   | Variable Index |
| Constant        | -                                   | Constant Index |
| Endline         | -                                   | - |
| Predefine       | State                               | Value |
|                 | Type                                | All data types |
|                 | Index                               | Up to 16-bit limit uint |
|                 | Char                                | Full ASCII (8-bit) |
|                 | Math operation                      | ... |
|                 | Bool                                | True/False |
|                 | Number                              | 16-bit Q8.8 fixed-point literal |

The editor checks the validity of a script: end matching, and type and size correctness.

The program is symbol-based, with one line per instruction. A line always follows the order Output, Instruction, Input, End, for example Out2, Var2, InsX, Var1, OpAdd, Var2, Const1, In3, EndLine.

The program is pre-loaded into RAM when space allows, using read-only pointers for fast access, checked at script start.

Execution happens in the main loop. All instructions run until the same instruction is reached again, which is a loop, or until the script waits; the state is checked again in the next loop update.
#### State
| State    | Definition |
| -------- | ---------- |
| Stopped  | Program is at instruction 0, waiting to be started. |
| Running  | Program is actively executing instructions. |
| Paused   | Program has been halted by the user, instruction counter above 0. |
| Waiting  | Program is waiting for a condition to be met, a timer or an external input. |
| Finished | Program has reached the final instruction. |
| Error    | Program has been stopped, the current instruction produced an error. |
#### Functions
- Generic math and logic processor
	 Var = (X+(Y-6)/Z^2 < P)&U
- Compose and extract functions
	`Vector`, `Matrix` or `Colour` plus an Index convert to and from `Number` or `uint8`.
- Register reader and writer
	Var, success = read (BlockInfo) / readforeign (Address, BlockInfo).
	Success = write (Val, BlockInfo) / writeforeign (Val, Address, BlockInfo).
	The writer uses a different TrID for each instance, and waits for confirmation.
- If/While blocks
	The math and logic processor is embedded in the input, and the expected result is a `Bool`.
	Terminated with a block end instruction.
- Time functions
	Delay, get time, and similar.
- State reader and writer
	Pause, resume, terminate, restart, report info, halt on error.
- Custom logs
- Macro call
- Script loading and unloading
### Management Commands (050x)
| Name                         | ID | Request                                             | Response                                                  | Note |
| ---------------------------- | -- | --------------------------------------------------- | --------------------------------------------------------- | ---- |
| Get currently loaded scripts | 0  | -                                                   | Fragmentation, Script File IDs (uint16) (stream) | Get actively loaded scripts |
| Load Script                  | 1  | Script File ID (uint16), Script (loaded) ID (uint8) | Success (bool)                                            | Load into active memory |
| Unload script                | 2  | Script (loaded) ID                                   |                                                           | Unload script from active memory |
| Read state                   | 3  | Script ID                                           | State, Last error code                                    | 0 = OK |
| Set state                    | 4  | Script ID, new state                                |                                                           | Clears error |
| Read internal state          | 5  | Script ID                                           | Instruction counter, Variable RAM                         | (editor debug) |
| Move to instruction          | 6  | Script ID, Instruction number                       |                                                           | (editor debug) |
| Write Variable               | 7  | Script ID, Variable ID, Value                       |                                                           | (editor debug) |
