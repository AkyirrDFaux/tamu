Each script is stored as separate file (max 256 scripts).
One implementation for all devices, prerequisite for implementation is dynamic and keyed memory.
Two services - Script manager (Status, Edits, IO, for user), Script instruction (Script interaction with other services, internal).
Use define USE_SCRIPTS
#### File blocks:

| Block part                    | Size/type                     | Note              |
| ----------------------------- | ----------------------------- | ----------------- |
| Name                          | `char[16]`                    |                   |
| Input count                   | uint8                         |                   |
| Output count                  | uint8                         |                   |
| Variable count                | uint8                         |                   |
| Constant count                | uint8                         |                   |
| Input definition meta length  | uint16                        |                   |
| Input definition value length | uint16                        |                   |
| Constant values length        | uint32                        |                   |
| Instruction length            | uint32                        |                   |
| Input meta                    | Input definition meta length  | Dictionary + keys |
| Input values                  | Input definition value length | Values of keys    |
| Output names                  | Output count * `char[16]`     |                   |
| Variable names                | Variable count * `char[16]`   |                   |
| Constant meta                 | Constant count * `BlockMeta`  |                   |
| Constant values               | Constant values length        |                   |
| Instructions                  | Instruction length            | Symbols           |

Inputs are stored like [[App/Service views/Keyed Memory|Keyed Memory]] (definitions of interaction, looks, i.e. buttons, toggles, pickers, variables), the real value in RAM is stored like [[Services/Dynamic Memory|Dynamic Memory]].
Variables and outputs work like [[Services/Dynamic Memory|Dynamic Memory]] blocks (stored in ram, size and type dynamic, but values never saved).
Constants are parts of file, work as read only variable
Predefines are commonly reused enums/tiny data, that are better compressed directly than stored as constants.

#### Symbol

| Type            | Subtype                             | Value                    |
| --------------- | ----------------------------------- | ------------------------ |
| uint8 (enum)    | uint8 (enum)                        | uint16                   |
| Instruction     | Subtype (Math/Service/Flow/Time...) | Instruction subtype enum |
| Input variable  | -                                   | Input Index              |
| Output variable | -                                   | Output Index             |
| Variable        | -                                   | Variable Index           |
| Constant        | -                                   | Constant Index           |
| Endline         | -                                   | -                        |
| Predefine       | State                               | Value                    |
|                 | Type                                | All data types           |
|                 | Index                               | 0-16bit limit uint       |
|                 | Char                                | Full ascii (8-bit)       |
|                 | Math op.                            | ...                      |
|                 | Bool                                | True/False               |


Editor has to check validity of script (end matching, function input/output type correctness).
Function output cannot be directly input of a function, only variable/const/predefine
Program is symbol based, with a line for each instruction. Line always follows Output-Instruction-Input-End. (Out2, Var2, InsX, Var1, OpAdd, Var2, Const1, In3, EndLine...)
Can be preloaded to ram if space is sufficient (use read only pointers for fast access, check on script start).
Execution done in main loop. Run all instructions until the same instruction reached (loops) or until waiting. Check again in next update.
#### State

| State    | Definition                                                                    |
| -------- | ----------------------------------------------------------------------------- |
| Stopped  | Program is at instruction 0, waiting to be started                            |
| Running  | Program is actively exectuing instructions                                    |
| Paused   | Program has been halted by user, instruction counter > 0                      |
| Waiting  | Program is waiting for condition to be met (timer/waiting for external input) |
| Finished | Program has reached final instruction                                         |
| Error    | Program has been stopped, current instruction has produced an error.          |
#### Functions
 - Generic math and logic processor
	 Var = (X+(Y-6)/Z^2 < P)&U
- Compose and Extract functions
	Vector/Matrix/Color + Index <=> Number/uint8
- System & Dynamic block reader and writer
	Var, success = fn(Address)
	Success = fn(Val, addr)
	Writer sets the script updated flag
	Uses Script instruction CID.
- If/While blocks
	M&L processor embedded in input, expected output bool.
	Terminated with end instruction
- Time functions
	Delay, Get time ...
- State reader and writer
	Pause, resume, terminate, restart, info report, error halt...
- Macro call 

### Service CIDs (manager)

| Function                   | CID | Payload In                                        | Payload out                       | Note           |
| -------------------------- | --- | ------------------------------------------------- | --------------------------------- | -------------- |
| Get number of scripts      | 0   | -                                                 | uint8                             |                |
| Read Name                  | 1   | Script ID                                         | `char[16]`                        |                |
| Read I/O size              | 2   | Script ID                                         | Input and output size (uint8 x 2) |                |
| Read state                 | 3   | Script ID                                         | State                             |                |
| Set state                  | 4   | Script ID, new state                              |                                   |                |
| Read input                 | 5   | Script ID, input index                            | BlockMeta, value                  |                |
| Write input                | 6   | Script ID, input index, padding, BlockMeta, value |                                   |                |
| Read output                | 7   | Script ID, output index                           | BlockMeta, value                  |                |
| Get info                   | 8   | Script ID                                         | Variable count, instruction count | (editor debug) |
| Read Variable              | 9   | Script ID, Variable ID                            | Block Meta, Value                 | (editor debug) |
| Write Variable             | 10  | Script ID, Variable ID, Block Meta, Value         |                                   | (editor debug) |
| Get current instruction    | 11  | Script ID                                         | instruction number                | (editor debug) |
| Move to instruction        | 12  | Script ID, instruction number                     |                                   | (editor debug) |
| Create script              | 13  | Script ID                                         | Success                           | (file access)  |
| Delete script              | 14  | Script ID                                         | Success                           | (file access)  |
| Read script                | 15  | Script ID                                         | Stream (binary)                   | (file access)  |
| Open script write stream   | 16  | Script ID, expected size (uint32)                 | CID of opened stream              | (file access)  |
| Close script  write stream | 17  | Script ID                                         |                                   | (file access)  |
| Write script stream        | 64+ | Stream (binary)                                   |                                   | (file access)  |
### Service CIDs (instruction I/O)
Service CID for script functionality is equal to ScriptID