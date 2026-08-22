Each script is stored as separate file
One implementation for all devices.

File blocks:
- Generic information (Name, IO count, block offsets)
- Input space
- Output space
- Variables
- Constants
- Instructions
Each script get's own service ID for I/O interactions.

Variables are stored in ram, size dynamic
Constants are parts of file, work as read only variable
Input & Output space definable (buttons, toggles, pickers, variables)

Editor has to check validity of program
Function output cannot be directly input of a function, only variable/const/predefine
Program is symbol based (Out2, Var2, Fn, Var1, OpAdd, Var2, Const1, In3, EndLine...)
Can be preloaded to ram is space is sufficient

 Functions
 - Generic math and logic processor
	 Var = (X+(Y-6)/Z^2 < P)&U
- Compose and Extract functions
	Vector/Matrix/Color + Index <=> Number
- System & Dynamic block reader and writer
	Var, success = fn(Address)
	Success = fn(Val, addr)
- If/While blocks
	M&L processor embedded in input
	Terminated with end instruction
- Time functions
	Delay, Get time ...
- Status function 
	Pause, resume, terminate, restart, error/info report...
- Macro call

To be implemeted and finished later