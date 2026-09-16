Use define USE_SCRIPTS

Variable, constant and I/O are type and size fixed.
Variables are in static RAM array inside.
Constants work as read only variable.
IO is in register (dynamic memory without persistance).
Input needs default values and specification for UI (switch/button, names).
Each script is stored as separate file (SCR_XXX). 

Scripts have to first be pre-loaded (register and variable space creation).
A table stores the currently loaded Script File IDs.
Another table stores the allocated sizes of variable space memory.
Variable space memory contains the instruction counter at the start.
Another table contains the loaded script states.
#### File blocks

| Block part              | Size/type              | Note    |
| ----------------------- | ---------------------- | ------- |
| Properties              | 32bit                  |         |
| Input count             | uint8                  |         |
| Output count            | uint8                  |         |
| Variable count          | uint8                  |         |
| Constant count          | uint8                  |         |
| Constants length        | uint32                 |         |
| Input defaults length   | uint32                 |         |
| Instruction length      | uint32                 |         |
| UI info size            | uint32                 |         |
| Input types and size    | N1 * ValueInfo (32bit) |         |
| Output types and size   | N2 * ValueInfo (32bit) |         |
| Variable types and size | N3 * ValueInfo (32bit) |         |
| Constant types and size | N4 * ValueInfo (32bit) |         |
| Constant values         | Constant values length |         |
| Input defaults          | Input defaults length  |         |
| Instructions            | Instruction length     | Symbols |
| UI info                 | UI info length         |         |

Properties: Information about script type (load on boot etc..)

UI info includes function name, input, output, variable and constant names, and input limits, and UI type.

Predefines are commonly reused enums/tiny data, that are better compressed in symbols directly than stored as constants.
#### Symbol

| Type (uint8 enum) | Subtype (uint8 enum)                | Value (uint16)           |
| ----------------- | ----------------------------------- | ------------------------ |
| Instruction       | Subtype (Math/Service/Flow/Time...) | Instruction subtype enum |
| Input variable    | -                                   | Input Index              |
| Output variable   | -                                   | Output Index             |
| Variable          | -                                   | Variable Index           |
| Constant          | -                                   | Constant Index           |
| Endline           | -                                   | -                        |
| Predefine         | State                               | Value                    |
|                   | Type                                | All data types           |
|                   | Index                               | 0-16bit limit uint       |
|                   | Char                                | Full ascii (8-bit)       |
|                   | Math op.                            | ...                      |
|                   | Bool                                | True/False               |


Editor has to check validity of script (end matching, type and size correctness).
Program is symbol based, with a line for each instruction. Line always follows Output-Instruction-Input-End. (Out2, Var2, InsX, Var1, OpAdd, Var2, Const1, In3, EndLine...)
Can be preloaded to RAM if space is sufficient (use read only pointers for fast access, check on script start).
Execution done in main loop. Run all instructions until the same instruction is reached (loops) or until waiting. Check again in next loop's update.
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
- Register reader and writer
	Var, success = read (blockinfo) / readforeign(Address, blockinfo).
	Success = write(Val, blockinfo) / writeforeign(Val, Address, blockinfo).
	Writer sets the script updated flag, uses different TRID for each instance.
	Waits for confirmation.
- If/While blocks
	M&L processor embedded in input, expected result bool.
	Terminated with block end instruction.
- Time functions
	Delay, Get time ...
- State reader and writer
	Pause, resume, terminate, restart, info report, error halt...
- Custom logs
- Macro call 
- Script (un)loading

### Management commands (0x050X)

| Function                     | CID | Payload In                    | Payload out                                       | Note                             |
| ---------------------------- | --- | ----------------------------- | ------------------------------------------------- | -------------------------------- |
| Get currently loaded scripts | 0   | -                             | Number of loaded scripts, Script File IDs (uint8) | Get actively loaded scripts      |
| Load Script                  | 1   | Script File ID                | Script (loaded) ID                                | Load into active memory          |
| Unload script                | 2   | Script (loaded) ID            |                                                   | Unload script from active memory |
| Read state                   | 3   | Script ID                     | State                                             |                                  |
| Set state                    | 4   | Script ID, new state          |                                                   |                                  |
| Read internal state          | 5   | Script ID                     | Instruction counter, Variable RAM                 | (editor debug)                   |
| Move to instruction          | 6   | Script ID, Instruction number |                                                   | (editor debug)                   |
| Write Variable               | 7   | Script ID, Variable ID, Value |                                                   | (editor debug)                   |
