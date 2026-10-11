Use define `USE_SCRIPTS`.

A script is a stored program that runs on the device, driven by a symbol-based instruction set executed in the main loop. Each script is stored as its own file, `SCR_XXX`, pre-loaded into RAM before it runs. A script that links global functions also needs the function library file present on the target.
### Variables, Constants and I/O
Variables, constants and I/O are fixed in type and size. Variables live in a static RAM array inside the script, with no reserved words ahead of them; constants work as read-only variables; I/O is a register, dynamic memory without persistence. Inputs carry a default value and a UI specification, such as whether they render as a switch or a button, and a name.
### File Format
#### File Blocks
| Name                    | Size                  | Note                                |
| ----------------------- | --------------------- | ----------------------------------- |
| Properties              | uint16                | A flags word                        |
| Input count             | uint8                 |                                     |
| Output count            | uint8                 |                                     |
| Variable count          | uint8                 |                                     |
| Constant count          | uint8                 |                                     |
| Function count          | uint8                 |                                     |
| Reserved                | uint8                 | Pads the counts to 4-byte alignment |
| Constants length        | uint32                | Bytes                               |
| Input defaults length   | uint32                | Bytes                               |
| Instruction length      | uint32                | Bytes                               |
| UI info size            | uint32                | Bytes                               |
| Input types and size    | ValueInfo[N1]         |                                     |
| Output types and size   | ValueInfo[N2]         |                                     |
| Variable types and size | ValueInfo[N3]         |                                     |
| Constant types and size | ValueInfo[N4]         |                                     |
| Constant values         | Constants length      |                                     |
| Input defaults          | Input defaults length |                                     |
| Function reservations   | Function count x 16   | Bytes                               |
| Instructions            | Instruction length    | Symbols                             |
| UI info                 | UI info length        | App-side only                       |
The header is 24 bytes. Every `ValueInfo` entry occupies its size rounded up to 4 bytes, and the arrays sit in the order input, output, variable and constant, so a value's file offset equals its offset in the run-time space; the constant values and input defaults blobs use the same packing. A function reservation is 16 bytes: the file a linked global function lives in, its memory-space offset and its instruction-space offset, one record per declared function. The UI info is decoded only by the app (the firmware treats it as opaque bytes); see [[App/Service Views/Script|the app]] for its format.
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
The Math op values form the operators of an inline expression. The expression holds up to 9 elements and nests to 8 levels.

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
### Program
#### Line Shape
The program is symbol-based, with one line per instruction. A line follows the order destination, instruction, operand, end: any number of destination symbols (outputs, variables or constants), then the instruction symbol, then any number of operand symbols (inputs, variables, constants or predefines), terminated by an endline symbol. One line, for example, is Out2, InsSet, Var1, OpAdd, Var2, Const1, In3, EndLine.
#### Execution
The program is pre-loaded into RAM when space allows, using read-only pointers for fast access, checked at script start. Execution happens in the main loop: all instructions run until the same instruction is reached again, which is a loop, or until the script waits, and the state is checked again in the next loop update.
#### State
| State                  | Value | Definition                                                                                                                                                                                                        |
| ---------------------- | ----- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Stopped                | 0     | Program is at instruction 0, waiting to be started.                                                                                                                                                               |
| Running                | 1     | Program is actively executing instructions.                                                                                                                                                                       |
| Paused                 | 2     | Program has been halted by the user, instruction counter above 0.                                                                                                                                                 |
| Waiting                | 3     | Program is waiting for a condition to be met, a timer or an external input.                                                                                                                                       |
| Finished               | 4     | Program has reached the final instruction.                                                                                                                                                                        |
| Error                  | 5     | Program has been stopped, the current instruction produced an error.                                                                                                                                              |
| Reserved-but-not-ready | 6     | The slot's space exists but the script cannot run yet; it leaves by becoming ready (`Stopped`) or by being released.                                                                                              |
| Load-failed            | 7     | The load was refused; the slot keeps its reservation until an explicit unload clears it. A link failure is detected before anything is allocated, so a load-failed slot holds only its slot record and no spaces. |
#### Validation
The editor checks a script before upload: block-end matching (every `If`/`While` has its `End block`), and type and size correctness.
### Instructions
An instruction symbol's `Subtype` is its category and its `Value` is the opcode within that category. An opcode keeps its number reserved when it is retired, and is never reused.

| Category | Value |
| -------- | ----- |
| Math     | 0     |
| Logic    | 1     |
| Flow     | 2     |
| Time     | 3     |
| Service  | 4     |
| Compose  | 5     |

#### Math
| Opcode | Name      | Note                                                |
| ------ | --------- | --------------------------------------------------- |
| 0      | Set       | Evaluates the infix expression; scalar, vector or matrix destination; the other math instructions fold their operands left to right |
| 1-4    | Reserved  | Add, Subtract, Multiply and Divide, retired         |
| 5      | Modulo    |                                                     |
| 6      | Minimum   |                                                     |
| 7      | Maximum   |                                                     |
| 8      | Reserved  | Neg, retired                                        |
| 9      | Absolute  |                                                     |
| 10     | Limit     | clamp(value, min, max)                              |
| 11     | Transform | 2x3 matrix from (rot, ox, oy, sx, sy[, skew])       |

#### Logic
| Opcode | Name   | Note                                                                                        |
| ------ | ------ | ------------------------------------------------------------------------------------------- |
| 12     | Select | condition, value if true, value if false; comparisons and logic operations are expression operators (a Math op predefine) |

#### Flow
| Opcode | Name      | Note                                                                  |
| ------ | --------- | --------------------------------------------------------------------- |
| 0      | If        | Input is the math and logic expression; the expected result is `Bool`  |
| 1      | While     | Input is the math and logic expression; the expected result is `Bool`  |
| 2      | End block | Terminates an If or While                                             |
| 3      | Jump      |                                                                       |
| 4      | Call      | The reuse mechanism for functions                                     |
| 5      | Return    |                                                                       |
| 6      | Halt      | Finishes the program                                                  |

#### Time
| Opcode | Name       | Note                                                                                         |
| ------ | ---------- | -------------------------------------------------------------------------------------------- |
| 0      | Delay      |                                                                                              |
| 1      | Wait until |                                                                                              |
| 2      | Get time   | Integer destination only (`Index`/`Uint32`); a `Number` destination is a type error          |

#### Service
| Opcode | Name                     | Note |
| ------ | ------------------------ | ---- |
| 0      | Log                      | A custom log: the entry's source is the whole 32-bit block index and its code a source-specific 32 bits ([[Services/Log Handler\|Log Handler]]) |
| 1      | Register read            | |
| 2      | Register write           | A different TrID per instance; waits for confirmation |
| 3      | Reserved                 | The retired state instruction; reserved and never reused |
| 4      | Nop                      | |
| 5      | Register read (foreign)  | |
| 6      | Register write (foreign) | |
| 7      | Load script              | Management CID 1; returns its code as a result instead of advancing silently |
| 8      | Unload script            | Management CID 2; returns its code as a result instead of advancing silently |
| 9      | Start                    | Entering `Running` resets the instruction counter and the call depth; restart resolves to `Start` |
| 10     | Stop                     | |
| 11     | Pause                    | |
| 12     | Resume                   | |
| 13     | Error                    | A custom error throw: logs and halts, carrying a script-defined code |
| 14     | Report info              | A log-only action: logs the script's state and carries on |
The state verbs are separate instructions, not values of one state instruction: the state enum describes conditions, while start, stop, pause and resume are actions on those conditions.

#### Compose
| Opcode | Name     | Note                                                                                         |
| ------ | -------- | -------------------------------------------------------------------------------------------- |
| 0      | Compose  | Builds a container from scalar values; `Vector`, `Matrix` and `Colour` with an index convert to and from `Number` or `uint8`; a `Text` element is a `Char` |
| 1      | Extract  | Reads one element back                                                                       |
### Error Model
An error is a 32-bit code, and a fault also names the instruction that produced it.
- Code 0 is universally "no error", for faults and results alike, so every instruction's code space starts at 1.
- A result is an error code, not a separate type: an external-facing instruction returns 0 for success, or an error code otherwise, in the same space a report uses.
- Errors are instruction-specific, with no class layer: a code's meaning is defined by the instruction that faulted, and the instruction field in the same record is what makes the code interpretable. The app looks the code up in that instruction's table; the human-readable names are the app's, nothing goes on the device and there is no mirroring contract.
- A failure surfaces two ways: the instruction carries its success flag out so a script can check it, and a failure that is not checked becomes an error.

A fault is an `(instruction, error)` pair:

| Instruction | Error | Meaning                                                              |
| ----------- | ----- | -------------------------------------------------------------------- |
| 0           | 0     | No error                                                             |
| 0           | N     | Engine-level fault: not any instruction's doing (0 is not a valid program instruction) |
| non-zero    | N     | The named instruction faulted; N is its instruction-specific code     |

Whether an instruction faults is decided by what it faces, not per call site:

| Instruction kind | Faults? | Behaviour |
| ---------------- | ------- | --------- |
| External-facing: register read/write (including the foreign variants) and the in-script `Load script`/`Unload script` | Never | Carries an error code into a result destination, 0 meaning success. The app always emits one - a scratch destination when the script ignores it - and the firmware requires it. |
| Internal: `Get time`, `Pow` | Always | Fails with a detailed error; a bad domain or a bad type is a programming error, not a condition to handle. |

`Pow` carries a specific detail on a domain failure: negative base with a fractional exponent, integer part above 64, or a zero reciprocal.

### Load Contract
| Phase | Fault | Detail |
| ----- | ----- | ------ |
| Load | Memory bounds | Computed from the file header (the declared input, output, variable and constant sizes, the constants and input-defaults lengths, and the instruction length) plus the sizes of the linked functions, then checked against available memory. |
| Load | Link resolution | A referenced function that is absent is a load error. |
| Run time | Unknown opcode, out-of-range index or operand, bad flow target, unresolvable reference | Raised when execution reaches it. A malformed script runs part-way and then stops, which is why the error carries the position of the failing instruction. |

- Two allocations per script, both made once at load and kept separate: the variable spaces (I/O, variables, constants) and the instruction (bytecode) space. Nothing allocates later.
- Everything the script runs on is copied into those allocations before the slot goes active, and the VM never aliases storage, so rewriting the stored file under a running script cannot corrupt it.
- Loading into an occupied slot fails: only empty slots accept a load, and a load that names no slot takes the lowest free one. A memory-bound failure leaves the slot inactive and the caller sees the failure; a link failure enters the `load-failed` state.
- Scripts are independent: a reference to a block that is not present is a run-time error on the read/write instruction, not a load failure and not a tracked dependency.

### Functions and Macros
- A macro is an authoring-layer feature of the app, not an instruction: the app expands it into a Flow `Call` and a body.
- Reuse is the Flow `Call`/`Return` - one body per script, a `Call` at each use site. Parameters are by value: the caller writes them into the body's variables before the call, and, with no stack frame, those variables are the parameter channel.
- Nested calls are allowed. The app refuses, at assembly, any cyclic call graph and any nesting deeper than the call-depth cap of 8, so the no-frame limits become build errors rather than run-time surprises. Recursion and re-entrancy are impossible by design and accepted.
- Local functions are laid out by the app from definitions kept in the file's app-only UI blob.
- Global functions are linked at load, not copied into the script file: the device reads the function file, places the referenced bodies' memory and instruction space into the script's allocations, and patches the calls. The function file is the script file format with the Function library flag, so a script using global functions is not self-contained - the function file must be present on the target.
- Calls are precompiled: the interface (inputs and outputs) is known at assembly, and only the address is filled in at load, by the same relocation.

### Register Representation
- A loaded script is addressed, not registered: nothing is stored, allocated or registered for a script, and it takes no part in the dynamic-block lifecycle.
- The Scripts range (0x3F4-0x3F7, four types x 64 instances = 256 slots) is a fixed set of block-type numbers at which a `BlockInfo` names a script's I/O. The Register dispatcher forwards an access in that range to the Script service, and the block's metadata is synthesised by the Script service from the script's own.
- Only I/O (inputs and outputs) is exposed; variables and constants stay off the register path and are reached through the management CIDs.
- The type numbers are unconditional constants; only the routing is gated by `USE_SCRIPTS`. The slot count is a per-target decision, since the registry is static, so the app queries the slot count rather than assuming 256. See [[Services/Register|Register]].
- References to blocks stay positional `BlockInfo` (`Type.Instance.Field.Key`); there is no name binding and no file versioning, so a script file is portable in the weak sense only - it loads and runs on another device, but the device's software version is the compatibility signal.
### Management Commands (050x)
| Name                         | ID  | Request                                             | Response                                                            | Note                                                                                                                |
| ---------------------------- | --- | --------------------------------------------------- | ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| Get currently loaded scripts | 0   | -                                                   | Fragmentation, slot map (uint16 per slot)                           | The whole map in slot order; a file id per slot, `0xFFFF` for an empty slot; the stream length gives the slot count |
| Load Script                  | 1   | Script File ID (uint16), Script (loaded) ID (uint8) | Success flag                                                        | Fails on an occupied slot                                                                                           |
| Unload script                | 2   | Script (loaded) ID                                  | Success flag                                                        | Reports success or failure only                                                                                     |
| Read state                   | 3   | Script ID                                           | Report record                                                       | See Report Record                                                                                                   |
| Set state                    | 4   | Script ID, action                                   | Success flag                                                        | The action enum drives start, stop, pause and resume; clears the error                                              |
| Read internal state          | 5   | Script ID                                           | Fragmentation, <br>Instruction-counter stack, Variable RAM (stream) | Editor debug; fragmented                                                                                            |
| Move to instruction          | 6   | Script ID, Line index (uint32)                      |                                                                     | Editor debug                                                                                                        |
| Write Variable               | 7   | Script ID, Variable ID, Value                       | Success flag                                                        | Editor debug                                                                                                        |
In CIDs 3-7, the script id is the loaded slot id; the file id and the slot are independent. CID 0's map is the file-to-slot mapping. CIDs 0 and 5 both exceed one frame and stream through the [[Services/App Interface|app interface]]'s fragmentation.
#### Report Record
CID 3 returns the report record. The depth byte pads the record so `line` sits on a 2-byte boundary and the error's two 32-bit fields land on 4-byte boundaries.

| Name        | Size   | Note                                                            |
| ----------- | ------ | --------------------------------------------------------------- |
| State       | uint8  | Script state                                                    |
| Depth       | uint8  | Call depth                                                      |
| Line        | uint16 | The instruction counter, the deepest frame's position           |
| Instruction | uint32 | Only on an error: the offending instruction's own encoding       |
| Error       | uint32 | Only on an error: the instruction-specific error code            |
There is no separate symbol field: the error's instruction-specific detail carries it. CID 5 returns the variable RAM and the entire instruction-counter call stack.
