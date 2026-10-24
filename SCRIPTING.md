# Tamu Scripting

A script is a stored program that runs on the device: a symbol-based instruction set executed in
the main loop, built in the app's script editor. Each script is its own file, `SCR_XXX`,
pre-loaded into RAM before it runs; a script that links global functions also needs the function
library file present on the target. The engine is behind the `USE_SCRIPTS` define.

Scripts live in slots. A load into an occupied slot fails, a load that names no slot takes the
lowest free one, and the loaded slot id and the file id are independent. A slot can hold a script
that cannot run yet (`Reserved-but-not-ready`) or one whose load was refused (`Load-failed`, which
keeps the slot record until an explicit unload). A file marked as a function library is not a
script.

The image carries the counts and lengths, the type and size of every input, output, variable and
constant, the constant values, the input defaults, the function reservations, the instruction
stream and an app-only UI blob. Two allocations are made once at load and never grow: the variable
spaces (I/O, variables, constants) and the instruction space. Everything the script runs on is
copied in before the slot goes active and the VM never aliases storage, so rewriting the stored
file under a running script cannot corrupt it.

Variables are static RAM inside the script, fixed in type and size, with no reserved words ahead of
them; constants are read-only variables; I/O is a register, dynamic memory without persistence.
Inputs carry a default value, a UI specification (whether they render as a switch or a button) and
a name. Only I/O is exposed on the register path; variables and constants are reached through the
management commands (050x).

| Flag | Description |
| --- | --- |
| Load on boot (bit 0) | Loads the script at boot. |
| Run on load (bit 1) | Runs the script as soon as it is loaded. |
| Function library (bit 2) | The file is a function library, not a script. |
| Slot (bits 8-15) | The uint8 slot a load-on-boot script loads into; a boot load is deterministic rather than the identity slot or the first free one. |

The specification is `Docs/Services/Script.md`. This file is the manual for it.

## Operations

A program is symbol-based, one line per instruction. A line runs destination, instruction,
operand, end: any number of destination symbols (outputs, variables or constants), then the
instruction symbol, then any number of operand symbols (inputs, variables, constants or
predefines), terminated by an endline symbol. One line is
`Out2, InsSet, Var1, OpAdd, Var2, Const1, In3, EndLine`.

An instruction symbol's `Subtype` is its category and its `Value` is the opcode within that
category. A retired opcode keeps its number reserved and is never reused.

| Category | Value |
| --- | --- |
| Math | 0 |
| Logic | 1 |
| Flow | 2 |
| Time | 3 |
| Service | 4 |
| Compose | 5 |

Where the specification's table names an instruction's operands they are worded as it words them;
where it does not, the operands are the roles the editor's instruction set defines, which is what
the symbol picker recommends. A dash means no operand.

### Math

| Opcode | Name | Operands | Meaning |
| --- | --- | --- | --- |
| 0 | Set | an infix expression | Evaluates the expression; the destination is scalar, vector or matrix. |
| 1-4 | Reserved | - | Add, Subtract, Multiply and Divide, retired. |
| 5 | Modulo | value, modulus | Integer remainder; a zero modulus fails. |
| 6 | Minimum | value, value, ... | The smaller of its operands. |
| 7 | Maximum | value, value, ... | The larger of its operands. |
| 8 | Reserved | - | Neg, retired. |
| 9 | Absolute | value | Magnitude, sign removed. |
| 10 | Limit | value, min, max | `clamp(value, min, max)`. |
| 11 | Transform | rot, offset X, offset Y, scale X, scale Y [, skew] | A 2x3 matrix from those six values. |

Every Math instruction apart from `Set` folds its operands left to right.

### Logic

| Opcode | Name | Operands | Meaning |
| --- | --- | --- | --- |
| 12 | Select | condition, value if true, value if false | Picks one of two values; comparisons and logic operations are expression operators, a Math op predefine. |

### Flow

| Opcode | Name | Operands | Meaning |
| --- | --- | --- | --- |
| 0 | If | condition | Runs the block below when the expression holds; the input is the math and logic expression and the expected result is `Bool`. |
| 1 | While | condition | Repeats the block while the expression holds; the input is the math and logic expression and the expected result is `Bool`. |
| 2 | End block | - | Terminates an If or While. |
| 3 | Jump | line | Continues at another line. |
| 4 | Call | line | The reuse mechanism for functions. |
| 5 | Return | - | Leaves the current function. |
| 6 | Halt | - | Finishes the program. |

### Time

| Opcode | Name | Operands | Meaning |
| --- | --- | --- | --- |
| 0 | Delay | ms | Waits for the given time. |
| 1 | Wait until | condition | Holds the script in `Waiting` until the condition is met. |
| 2 | Get time | - | Reads the device time; an integer destination only (`Index`/`Uint32`), a `Number` destination is a type error. |

### Service

| Opcode | Name | Operands | Meaning |
| --- | --- | --- | --- |
| 0 | Log | code | A custom log: the entry's source is the whole 32-bit block index and its code a source-specific 32 bits (`Docs/Services/Log Handler.md`). |
| 1 | Register read | register | Reads a block field into a destination; the register is a `BlockInfo` constant. |
| 2 | Register write | register, value | Writes a block field; a different TrID per instance, and the instruction waits for confirmation. |
| 3 | Reserved | - | The retired state instruction; reserved and never reused. |
| 4 | Nop | - | Does nothing. |
| 5 | Register read (foreign) | device, register | Reads a block field on another device. |
| 6 | Register write (foreign) | device, register, value | Writes a block field on another device. |
| 7 | Load script | file, script | Management CID 1; returns its code as a result instead of advancing silently. |
| 8 | Unload script | script | Management CID 2; returns its code as a result instead of advancing silently. |
| 9 | Start | - | Entering `Running` resets the instruction counter and the call depth; restart resolves to `Start`. |
| 10 | Stop | - | Stops the script. |
| 11 | Pause | - | Halts the script by request, instruction counter above 0. |
| 12 | Resume | - | Resumes a paused script. |
| 13 | Error | code | A custom error throw: logs and halts, carrying a script-defined code. |
| 14 | Report info | - | A log-only action: logs the script's state and carries on. |

The state verbs are separate instructions, not values of one state instruction: the state enum
describes conditions, while start, stop, pause and resume are actions on those conditions.

### Compose

| Opcode | Name | Operands | Meaning |
| --- | --- | --- | --- |
| 0 | Compose | index, value, ... | Builds a container from scalar values; `Vector`, `Matrix` and `Colour` with an index convert to and from `Number` or `uint8`; a `Text` element is a `Char`. |
| 1 | Extract | container, index | Reads one element back. |

## Values and Operands

A symbol is 4 bytes: `Type` and `Subtype` are `uint8`, `Value` is a little-endian `uint16`.

| Type | Id | Subtype | Value |
| --- | --- | --- | --- |
| Instruction | 0 | Math / Logic / Flow / Time / Service / Compose | Opcode within the category |
| Input variable | 1 | - | Input index |
| Output variable | 2 | - | Output index |
| Variable | 3 | - | Variable index |
| Constant | 4 | - | Constant index |
| Endline | 5 | - | - |
| Predefine | 6 | State / Type / Index / Char / Math op / Bool / Number | Predefine value |

A predefine is a commonly reused enum or small datum, which compresses better as a symbol than as
a stored constant.

| Subtype | Id | Value |
| --- | --- | --- |
| State | 0 | State enum value |
| Type | 1 | A data type in `Docs/Data Formats.md` |
| Index | 2 | Up to the 16-bit limit uint |
| Char | 3 | Full ASCII, 8bit |
| Math op | 4 | An operator value below |
| Bool | 5 | True/False |
| Number | 6 | Q8.8 fixed-point literal: a sub-resolution value, since a wider one cannot fit in 16 bits |

A register operand is a constant of type `BlockInfo`. The editor shows a variable, an input or an
output by its name where the file carries one, an instruction as the category and its name, and a
Math op as its sign.

## Expressions

An expression is the operand stream of `Set`, `If`, `While` and Time `Wait until`. It holds up to
9 elements and nests to 8 levels. Its operators are the Math op predefine values.

| Value | Operator | Note |
| --- | --- | --- |
| 0 | Add | Arithmetic; element-wise, a scalar broadcasts |
| 1 | Subtract | Arithmetic; element-wise, a scalar broadcasts |
| 2 | Multiply | Arithmetic; element-wise, a scalar broadcasts |
| 3 | Divide | Arithmetic; element-wise, a scalar broadcasts; a zero divisor fails |
| 4 | Modulo | Arithmetic; integer operands; a zero modulus fails |
| 5 | Power | Arithmetic; exponent integer part up to 64; no fractional power of a negative base |
| 6 | AND | Scalar operands only; result `Bool` |
| 7 | OR | Scalar operands only; result `Bool` |
| 8 | XOR | Scalar operands only; result `Bool` |
| 9 | NOT | Scalar operand only; result `Bool` |
| 10-11 | Reserved | Shift, dropped |
| 12 | Compare = | Scalar operands only; result `Bool` |
| 13 | Compare != | Scalar operands only; result `Bool` |
| 14 | Compare < | Scalar operands only; result `Bool` |
| 15 | Compare <= | Scalar operands only; result `Bool` |
| 16 | Compare > | Scalar operands only; result `Bool` |
| 17 | Compare >= | Scalar operands only; result `Bool` |
| 18 | Open parenthesis | Groups the expression |
| 19 | Close parenthesis | Groups the expression |
| 20 | dot | Vector operands of equal length; result `Number` |
| 21 | cross | `Vector3` by `Vector3`; result `Vector3` |
| 22 | size | Vector norm; result `Number` |
| 23 | transpose | Matrix R x C to C x R; up to 9 elements |

## Block Addressing

Memory is addressed through a 32-bit `BlockInfo`, split into three levels: block and instance,
then field and key.

| Part | Size |
| --- | --- |
| BlockType | 10bit |
| BlockInstance | 6bit |
| Field | uint8 |
| Key | uint8 |

A register read or write names its target by a constant holding that whole 32-bit index
(`Type.Instance.Field.Key`). References stay positional: there is no name binding and no file
versioning, so a script file is portable in the weak sense only, and the device's software version
is the compatibility signal.

The Scripts range (0x3F4-0x3F7, four types by 64 instances) is a fixed set of block types at which
a block's metadata names a script's I/O; the register dispatcher forwards an access in that range
to the Script service. A reference to a block that is not present is a run-time error on the
read/write instruction, not a load failure and not a tracked dependency.

Register read and write, including the foreign variants, are external-facing: they never fault,
they carry an error code into a result destination with 0 meaning success, and the app always
emits that destination. A write takes a different TrID per instance and waits for confirmation.
The foreign variants name the other device first, by an address.

## Common Patterns

Symbols are written as the specification writes them: `In3`, `Out2`, `Var1`, `Const0`, `Ins<Name>`
for an instruction, `Op<Operator>` for a Math op predefine, `EndLine` for the terminator. The
editor shows the same line with each value's name and each instruction's category.

A gate — pass an input through while an enable is true, otherwise hold a fixed value. `Select`
needs no block and keeps no state:

    Out0, InsSelect, In1, In0, Const0, EndLine

An edge latch — fire once on a rising edge, by comparing the input with the level it last had:

    Out0, InsSet, In0, OpAnd, OpNot, Var0, EndLine
    Var0, InsSet, In0, EndLine

`Var0` holds the level the input had when those lines last ran, so the first line is true only on
a change from false to true: `Out0` is the input AND NOT the stored level. The second line stores
the level for the next pass. `NOT` is a prefix operator over its single scalar operand.

`Select` is the choice where one of two values is wanted; `If` with `End block` is the choice
where several lines share a condition.
