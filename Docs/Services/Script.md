Use define `USE_SCRIPTS`.

A script is a stored program that runs on the device, driven by a symbol-based instruction set executed in the main loop. Each script is stored as its own file, `SCR_XXX`, pre-loaded into RAM before it runs. A script that links global functions also needs the function library file present on the target.
### Variables, Constants and I/O
Variables, constants and I/O are fixed in type and size. Variables live in a static RAM array inside the script, with no reserved words ahead of them; constants work as read-only variables; I/O is a register, dynamic memory without persistence. Inputs carry a default value and a UI specification, such as whether they render as a switch or a button, and a name.
### File Format
#### File Blocks
| Name                    | Size                   | Note            |
| ----------------------- | ---------------------- | --------------- |
| Properties              | uint16                 | A flags word |
| Input count             | uint8                  |                 |
| Output count            | uint8                  |                 |
| Variable count          | uint8                  |                 |
| Constant count          | uint8                  |                 |
| Function count          | uint8                  |                 |
| Reserved                | uint8                  | Pads the counts to 4-byte alignment |
| Constants length        | uint32                 | Bytes           |
| Input defaults length   | uint32                 | Bytes           |
| Instruction length      | uint32                 | Bytes           |
| UI info size            | uint32                 | Bytes           |
| Input types and size    | ValueInfo[N1]          |                 |
| Output types and size   | ValueInfo[N2]          |                 |
| Variable types and size | ValueInfo[N3]          |                 |
| Constant types and size | ValueInfo[N4]          |                 |
| Constant values         | Constants length       |                 |
| Input defaults          | Input defaults length  |                 |
| Instructions            | Instruction length     | Symbols         |
| Function reservations   | Function count x 16    | Bytes           |
| UI info                 | UI info length         | App-side only   |
The header is 24 bytes: `Properties` (uint16), the five counts and one padding byte, then the four lengths; the metadata arrays and the blobs follow in the order above. Every `ValueInfo` entry occupies its size rounded up to 4 bytes, and the arrays sit in the order input, output, variable and constant, so a value's file offset equals its offset in the run-time space; the constant values and input defaults blobs use the same packing. A function reservation is 16 bytes: the file a linked global function lives in, its memory-space offset and its instruction-space offset, one record per declared function. The UI info is decoded only by the app (the firmware treats it as opaque bytes); see [[App/Service Views/Script|the app]] for its format.
#### Properties
| Flag | Description |
| ---------- | ----------- |
| Load on boot (bit 0) | Loads the script at boot. |
| Run on load (bit 1) | Runs the script as soon as it is loaded. |
| Function library (bit 2) | The file is a function library, not a script. |
| Slot (bits 8-15) | The uint8 slot a load-on-boot script loads into. A boot load is deterministic rather than the identity slot or the first free one. |
#### Symbol
A symbol is 4 bytes: `Type` and `Subtype` are `uint8`, and `Value` is a little-endian `uint16`.

| Type            | Id  | Subtype                             | Value                   |
| --------------- | --- | ----------------------------------- | ----------------------- |
| Instruction     | 0   | Math / Logic / Flow / Time / Service / Compose | Opcode within the category |
| Input variable  | 1   | -                                   | Input index             |
| Output variable | 2   | -                                   | Output index            |
| Variable        | 3   | -                                   | Variable index          |
| Constant        | 4   | -                                   | Constant index          |
| Endline         | 5   | -                                   | -                       |
| Predefine       | 6   | State / Type / Index / Char / Math op / Bool / Number | Predefine value         |
Predefines are commonly reused enums and small data, which compress better as symbols than as stored constants. Their subtypes:

| Subtype   | Id  | Value                              |
| --------- | --- | ---------------------------------- |
| State     | 0   | State enum value                   |
| Type      | 1   | A data type in [[Data Formats]]    |
| Index     | 2   | Up to the 16-bit limit uint        |
| Char      | 3   | Full ASCII (8-bit)                 |
| Math op   | 4   | A Math op value below              |
| Bool      | 5   | True/False                         |
| Number    | 6   | Q8.8 fixed-point literal, a sub-resolution value: a wider one cannot fit in 16 bits |
The Math op values form the operators of an inline expression. The expression holds up to 9 elements and nests to 8 levels; values 10 and 11 (shifts) are reserved and never reused.

| Value | Operator          | Note |
| ----- | ----------------- | ---- |
| 0     | Add               | Arithmetic; element-wise, a scalar broadcasts |
| 1     | Subtract          | Arithmetic; element-wise, a scalar broadcasts |
| 2     | Multiply          | Arithmetic; element-wise, a scalar broadcasts |
| 3     | Divide            | Arithmetic; element-wise, a scalar broadcasts; a zero divisor fails |
| 4     | Modulo            | Arithmetic; integer operands; a zero modulus fails |
| 5     | Power             | Arithmetic; exponent integer part up to 64; no fractional power of a negative base |
| 6     | AND               | Scalar operands only; result Bool |
| 7     | OR                | Scalar operands only; result Bool |
| 8     | XOR               | Scalar operands only; result Bool |
| 9     | NOT               | Scalar operand only; result Bool |
| 10-11 | Reserved (shift, dropped) |  |
| 12    | Compare =         | Scalar operands only; result Bool |
| 13    | Compare !=        | Scalar operands only; result Bool |
| 14    | Compare <         | Scalar operands only; result Bool |
| 15    | Compare <=        | Scalar operands only; result Bool |
| 16    | Compare >         | Scalar operands only; result Bool |
| 17    | Compare >=        | Scalar operands only; result Bool |
| 18    | Open parenthesis  | Groups the expression |
| 19    | Close parenthesis | Groups the expression |
| 20    | dot               | Vector operands of equal length; result Number |
| 21    | cross             | Vector3 by Vector3; result Vector3 |
| 22    | size              | Vector norm; result Number |
| 23    | transpose         | Matrix R x C to C x R; up to 9 elements |
The editor checks the validity of a script: block-end matching, and type and size correctness.
### Program
#### Line Shape
The program is symbol-based, with one line per instruction. A line follows the order destination, instruction, operand, end: any number of destination symbols (outputs, variables or constants), then the instruction symbol, then any number of operand symbols (inputs, variables, constants or predefines), terminated by an endline symbol. One line, for example, is Out2, InsSet, Var1, OpAdd, Var2, Const1, In3, EndLine.
#### Execution
The program is pre-loaded into RAM when space allows, using read-only pointers for fast access, checked at script start. Execution happens in the main loop: all instructions run until the same instruction is reached again, which is a loop, or until the script waits, and the state is checked again in the next loop update.
#### State
| State    | Value | Definition                                                          |
| -------- | ----- | ------------------------------------------------------------------ |
| Stopped  | 0     | Program is at instruction 0, waiting to be started.                |
| Running  | 1     | Program is actively executing instructions.                        |
| Paused   | 2     | Program has been halted by the user, instruction counter above 0.  |
| Waiting  | 3     | Program is waiting for a condition to be met, a timer or an external input. |
| Finished | 4     | Program has reached the final instruction.                         |
| Error    | 5     | Program has been stopped, the current instruction produced an error. |
Two load-related members are added, their values not yet assigned. In `reserved-but-not-ready` the slot's space exists but the script cannot run yet; it leaves by becoming ready (`Stopped`) or by being released. In `load-failed` the load was refused, and the slot keeps its reservation in this state until an explicit unload clears it, so the failure stays visible per slot. A link failure is detected before anything is allocated, so a load-failed slot holds only its slot record and no spaces.
#### Instructions
An instruction symbol's `Subtype` is its category and its `Value` is the opcode within that category. An opcode keeps its number reserved when it is retired, and is never reused.

| Category | Value |
| -------- | ----- |
| Math     | 0     |
| Logic    | 1     |
| Flow     | 2     |
| Time     | 3     |
| Service  | 4     |
| Compose  | 5     |
Math evaluates the infix expression of a `Set` line, the generic math and logic processor, for example Var = (X+(Y-6)/Z^2 < P)&U; the other math instructions fold their operands left to right.

| Opcode | Name      | Note                                                |
| ------ | --------- | --------------------------------------------------- |
| 0      | Set       | Evaluates the infix expression; scalar, vector or matrix destination |
| 1-4    | Reserved  | Add, Subtract, Multiply and Divide, retired         |
| 5      | Modulo    |                                                     |
| 6      | Minimum   |                                                     |
| 7      | Maximum   |                                                     |
| 8      | Reserved  | Neg, retired                                        |
| 9      | Absolute  |                                                     |
| 10     | Limit     | clamp(value, min, max)                              |
| 11     | Transform | 2x3 matrix from (rot, ox, oy, sx, sy[, skew])       |
Logic holds only `Select`; comparisons and logic operations are operators of the expression (the Math op predefine).

| Opcode | Name   | Note                              |
| ------ | ------ | --------------------------------- |
| 12     | Select | condition, value if true, value if false |
Flow carries the blocks and the call structure. The math and logic processor is embedded in an `If` or `While` input, and the expected result is a `Bool`, terminated with `End block`. `Call` and `Return` are the reuse mechanism for functions.

| Opcode | Name      | Note                        |
| ------ | --------- | --------------------------- |
| 0      | If        |                             |
| 1      | While     |                             |
| 2      | End block | Terminates an If or While   |
| 3      | Jump      |                             |
| 4      | Call      |                             |
| 5      | Return    |                             |
| 6      | Halt      | Finishes the program        |
Time holds the delays and the time source. `Get time` takes an integer destination only (`Index`/`Uint32`); a `Number` destination is refused as a type error, because its range cannot represent the value.

| Opcode | Name       | Note |
| ------ | ---------- | ---- |
| 0      | Delay      |      |
| 1      | Wait until |      |
| 2      | Get time   |      |
Service reaches the outside world and the script's own lifecycle. The register writer uses a different TrID for each instance and waits for confirmation. The in-script `Load script` and `Unload script` are the same operations as the management CIDs 1 and 2, and return their code as a result instead of advancing silently.

| Opcode | Name                        | Note                              |
| ------ | --------------------------- | --------------------------------- |
| 0      | Log                         | A custom log                      |
| 1      | Register read               |                                   |
| 2      | Register write              |                                   |
| 3      | Reserved                    | The retired state instruction     |
| 4      | Nop                         |                                   |
| 5      | Register read (foreign)     |                                   |
| 6      | Register write (foreign)    |                                   |
| 7      | Load script                 |                                   |
| 8      | Unload script               |                                   |
| 9      | Start                       | Covers restart: entering `Running` already resets the instruction counter and the call depth |
| 10     | Stop                        |                                   |
| 11     | Pause                       |                                   |
| 12     | Resume                      |                                   |
| 13     | Error                       | A custom error throw: logs and halts, carrying a script-defined code |
| 14     | Report info                 | A log-only action: logs the script's state and carries on |
The state verbs are separate instructions, not values of one state instruction: the state enum describes conditions, while start, stop, pause and resume are actions, and they act on the states that already exist, so no new states are added. The retired state instruction is Service opcode 3, reserved and never reused. Finishing is the existing Flow `Halt`, restart resolves to `Start`, and halt-on-error is the faulting rule. The `Error` throw and the `Report info` action write a log entry, under the log-struct change below.

The log paths - the in-script `Log` op, the `Error` throw and the `Report info` action - rely on a Log-service change ([[Services/Log Handler|Log Handler]]): the entry's source field becomes 32 bits, the whole block index, that is block type, instance and the 16-bit field/key specifier, so an entry names the block that emitted it, and the log code becomes 32 bits, its meaning source-specific, the same principle as the instruction-specific errors.

Compose builds a container from scalar values and reads one element back. `Vector`, `Matrix` and `Colour` plus an index convert to and from `Number` or `uint8`; a `Text` element is a `Char`, not a numeric value.

| Opcode | Name     | Note |
| ------ | -------- | ---- |
| 0      | Compose  |      |
| 1      | Extract  |      |
### Error Model
- Code 0 is universally "no error", for faults and for results alike, so every instruction's code space starts at 1; a result is an error code, not a separate type, so an external-facing instruction returns 0 for success or an error code otherwise, in the same space the report uses.
- A fault is an `(instruction, error)` pair. Instruction 0 is not a valid program instruction: it says the fault has no instruction behind it, so `(0, 0)` is no error and `(0, N)` is an engine-level fault, which covers the faults that are not any instruction's doing. Errors are instruction-specific, with no class layer: the 32-bit error code's meaning is defined by the instruction that faulted, and the instruction field in the same record is what makes it interpretable - the app looks the code up in that instruction's table.
- A failure surfaces two ways, both wanted: the instruction carries its success flag out so a script checks it further down, and a failure that is not checked becomes an error.
- The per-instruction code catalogue - the engine-level codes that report with instruction 0, the codes for each instruction that faults, and the result codes the external-facing instructions return - is not written yet. TBD.

Whether an instruction faults is decided by what it faces, not per call site. External-facing instructions (register read and write including the foreign variants, and the in-script `Load script`/`Unload script`) never fault; they carry an error code into a result destination, 0 meaning success, so the app always emits one, a scratch destination when the script ignores it, and the firmware requires it. Internal instructions (`Get time`, `Pow`) always fault, with a detailed error: a bad domain or a bad type is a programming error, not a condition to handle. `Pow` carries a specific detail on a domain failure - negative base with a fractional exponent, integer part above 64, zero reciprocal.
### Load Contract
- Validation at load is memory bounds and link resolution: the requirement is computed from the file header - the declared input, output, variable and constant sizes, the constants and input-defaults lengths, and the instruction length - plus the sizes of the functions it links, and checked against available memory, and a referenced function that is absent is a load error. Every other fault - unknown opcode, out-of-range index or operand, bad flow target, unresolvable reference - is a run-time error, raised when execution reaches it, and a malformed script therefore runs part-way and then stops, which is why the error carries the position of the failing instruction.
- Two allocations per script, both made once at load: the variable spaces (I/O, variables, constants) and the instruction (bytecode) space, kept separate; nothing allocates later. Everything the script runs on is copied into those allocations before the slot goes active, and the VM never aliases storage, so rewriting the stored file under a running script cannot corrupt it.
- Loading into an occupied slot fails; only empty slots accept a load, and a load that names no slot takes the lowest free one. A memory-bound failure leaves the slot inactive and the caller sees the failure; a link failure enters the `load-failed` state.
### Functions and Macros
- A macro is an authoring-layer feature of the app, not an instruction: the app expands it into a Flow `Call` and a body. Reuse is the existing Flow `Call`/`Return`, one body per script and a `Call` at each use site, so no new VM instruction is needed; parameters are by value, the caller writing them into the body's variables before the call, so, with no stack frame, the body's variables are its parameter channel.
- Nested calls are allowed. The app refuses, at assembly, any cyclic call graph and any nesting deeper than the call-depth cap of 8, so the no-frame limits become build errors rather than run-time surprises. Recursion and re-entrancy are impossible by design and accepted.
- Local functions are laid out by the app from definitions kept in the file's app-only UI blob. Global functions are linked at load, not copied into the script file: the device reads the function file, places the referenced bodies' memory and instruction space into the script's allocations, and patches the calls. The function file is the script file format with the Function library flag, so a script using global functions is no longer self-contained: it needs the function file present on the target.
- Calls are precompiled: the interface (inputs and outputs) is known at assembly, and only the address is filled in at load, by the same relocation.
### Register Representation
A loaded script is addressed, not registered. The Scripts range (0x3F4-0x3F7, four types x 64 instances = 256 slots) is a fixed set of block-type numbers at which a `BlockInfo` names a script's I/O, and the Register dispatcher forwards an access in that range to the Script service; nothing is stored, allocated or registered for a script, and it takes no part in the dynamic-block lifecycle. Only I/O (inputs and outputs) is exposed; variables and constants stay off the register path and are reached through the management CIDs. The block's metadata is synthesised by the Script service from the script's own. The type numbers are unconditional constants; only the routing is gated by `USE_SCRIPTS`. See [[Services/Register|Register]]. The slot count is a per-target decision, since the registry is static, so the app queries the slot count rather than assuming 256.
### Management Commands (050x)
| Name                         | ID | Request                                             | Response                                  | Note                                                              |
| ---------------------------- | -- | --------------------------------------------------- | ----------------------------------------- | ----------------------------------------------------------------- |
| Get currently loaded scripts | 0  | -                                                   | Fragmentation, slot map (uint16 per slot) | The whole map in slot order; a file id per slot, `0xFFFF` for an empty slot; the stream length gives the slot count |
| Load Script                  | 1  | Script File ID (uint16), Script (loaded) ID (uint8) | Success flag                              | Fails on an occupied slot                                         |
| Unload script                | 2  | Script (loaded) ID                                   | Success flag                              | Reports success or failure only                                   |
| Read state                   | 3  | Script ID                                           | Report record                             | See Report Record                                                 |
| Set state                    | 4  | Script ID, action                                   | Success flag                              | The action enum drives start, stop, pause and resume; clears the error |
| Read internal state          | 5  | Script ID                                           | Variable RAM, instruction-counter stack (stream) | Editor debug; fragmented                                  |
| Move to instruction          | 6  | Script ID, Line index (uint32)                      |                                           | Editor debug                                                      |
| Write Variable               | 7  | Script ID, Variable ID, Value                       | Success flag                              | Editor debug                                                      |
In CIDs 3-7, the script id is the loaded slot id; the file id and the slot are independent. CID 0's map is the file-to-slot mapping. CIDs 0 and 5 both exceed one frame and stream through the [[Services/App Interface|app interface]]'s fragmentation.
#### Report Record
CID 3 returns the report record: 4 bytes, or 12 on an error. The depth byte was previously reserved; it pads the record so `line` sits on a 2-byte boundary and the error's two 32-bit fields land on 4-byte boundaries.

| Name        | Size   | Note                                                            |
| ----------- | ------ | --------------------------------------------------------------- |
| State       | uint8  | Script state                                                    |
| Depth       | uint8  | Call depth; was the reserved byte                               |
| Line        | uint16 | The instruction counter, the deepest frame's position           |
| Instruction | uint32 | Only on an error: the offending instruction's own encoding       |
| Error       | uint32 | Only on an error: the instruction-specific error code            |
There is no separate symbol field: the error's instruction-specific detail carries it. The error's `line` moves here from CID 5, and CID 5 keeps the rest of the internal picture. CID 5 returns the variable RAM and the entire instruction-counter call stack.
