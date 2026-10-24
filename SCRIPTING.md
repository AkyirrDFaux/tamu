# Tamu Scripting

A script is a stored program that runs on the device: a symbol-based instruction set executed in
the main loop, written in the app's script editor. Each script is its own file, `SCR_XXX`, stored
on the device and pre-loaded into RAM before it runs; a script that links global functions also
needs the function library file present on the target.

The specification is `Docs/Services/Script.md`. This file is the manual for it: what the editor
offers, what every operation expects in each operand position, and what it produces. The operations
listed here are exactly the operations the editor's instruction picker offers.

## Slots and Loading

A loaded script occupies a slot. The app's Scripts page lists the device's loaded slots and its
stored files; Load moves a file into a slot, Unload releases it, and Open editor edits the stored
file. A load that names no slot takes the lowest free slot, and a load into an occupied slot fails.
The loaded slot id and the file id are independent, so a file's number and the slot it runs in can
differ.

| Property | Meaning |
| --- | --- |
| Load on boot | The script is loaded when the device boots. |
| Boot slot | The slot a load-on-boot script loads into, which makes a boot load deterministic rather than the lowest free slot. |
| Run on load | The script runs as soon as it is loaded. |
| Function library | The file is a function library, not a script. |

A slot can hold a script that cannot run yet, or one whose load was refused; a refused load keeps
the slot record until an explicit unload clears it. A load fails when the script does not fit the
device's memory, or when a function it links from a library file is not on the device.

Uploading an edited script writes a new copy of the file. A running script is not disturbed: the
engine copies everything it runs on into the slot at load, so rewriting the stored file underneath
it cannot corrupt it.

## Declarations

Inputs, outputs, variables and constants are declared with a name, a type and a size, each type
described in `Docs/Data Formats.md`. The types are `Bool`, `Index`, `Number`, `Enum`, `Colour`,
`Vector`, `Matrix`, `Text`, `Filename`, `Uint32` and `BlockInfo`, and a declared type and size do
not change afterwards.

| Space | The script reads it | The script writes it | Other blocks reach it |
| --- | --- | --- | --- |
| Inputs | Yes | No | Yes, through the register |
| Outputs | Yes | Yes | Yes, through the register |
| Variables | Yes | Yes | No |
| Constants | Yes | No | No |

An input carries a default value, a name, and its UI specification: whether the app renders it as a
switch or as a button. Inputs and outputs are the script's I/O, exposed as register fields so that
blocks elsewhere on the device read and write them. Variables are the script's own state and keep
their value from pass to pass; constants are values fixed when the script is built.

Each space is a list indexed from 0, and the declaration counts are 8-bit: at most 255 entries in a
space.

## Values

A line names its values as symbols. The editor shows a declared value by its name where the
declaration carries one, tagged with its space and its index in that space: `In0`, `Out2`, `Var1`,
`Const0`.

| Kind | How the editor writes it | Limits |
| --- | --- | --- |
| Declared value | By name, tagged with the space and the index in that space | Up to 255 declarations per space, indexed from 0 |
| Number literal | A decimal, such as `0.5` or `-2.25` | 16-bit Q8.8: -128.00 to 127.996, in steps of 1/256 |
| Index literal | A whole number, such as `3` | 0 to 65535 |
| Char literal | One character | An 8-bit ASCII character |
| Bool literal | `true` or `false` | |
| State | A script state, by name | Stopped, Running, Paused, Waiting, Finished, Error |
| Type | A data type, by name | The types a declaration can take |
| Math op | An operator, by name | Only inside an expression |
| Block address | A block field picked in the block picker, held by a `BlockInfo` constant | See Block Addressing |
| Device address | `NetID.Device`, such as `0.8`, carried by an `Id` value or written as a number | A 16-bit packed address |
| Expression | Values and operators filling the operation's operand slots | Up to 9 elements, nesting to 8 levels |
| Line target | A whole number | The index of a line, 0 for the first, in a program of at most 1024 lines |

A destination is a variable or an output. An input and a constant are read-only, and the editor
refuses either one as a destination.

## The Program

The program is one line per operation. The editor shows a line as a card: the destination slot,
then the instruction, then one slot per operand with the operand's role written above it —
`condition`, `min`, `register`, `ms` and so on. That role is the position the operation expects its
operand in.

An operation that produces a value takes one destination; an operation that acts on the device or
on the program takes none. Before an upload the editor checks the destination, the number of
operands, the type of each operand and the block structure: every `If` and `While` is closed by an
`End block`, and an `End block` with no opener is an error.

The editor groups the operations into six categories and recommends a short set of operations for
the line being edited.

## Operations

An operand position is numbered from 1 and is the position the editor lays the operand out in. A
dash means the operation takes no operand.

### Math

| Operation | Operands | Output | What it does |
| --- | --- | --- | --- |
| Set | 1. expression | Destination: the result, a scalar, `Vector` or `Matrix` | Evaluates the expression and stores the result. Arithmetic is element-wise, and a scalar broadcasts across a vector or matrix. |
| Modulo | 1. value, 2. modulus, up to eight values | Destination: the remainder | The integer remainder of the first operand divided by the second, folded left to right through any further operands. A zero modulus fails. |
| Minimum | 1. value, 2. value, up to eight values | Destination: the smallest operand | Folds its operands from left to right. |
| Maximum | 1. value, 2. value, up to eight values | Destination: the largest operand | Folds its operands from left to right. |
| Absolute | 1. value | Destination: the magnitude | The value with its sign removed. |
| Limit | 1. value, 2. minimum, 3. maximum | Destination: the value inside the range | Holds the value inside the inclusive range: below the minimum it becomes the minimum, above the maximum the maximum. |
| Transform | 1. rotation, 2. offset X, 3. offset Y, 4. scale X, 5. scale Y, 6. skew (optional) | Destination: a `Matrix`, two rows by three columns | Builds an affine transform from those values. Rotation and skew are in radians; the offsets are the point the transform keeps in place. |

Every Math operation writes one destination and takes numeric operands. The destination's type
decides the arithmetic: `Number` is fixed point and keeps fractions, `Index` and `Uint32` are whole
numbers.

### Logic

| Operation | Operands | Output | What it does |
| --- | --- | --- | --- |
| Select | 1. condition, 2. value if true, 3. value if false | Destination: the chosen value | Picks one of two values by the condition, which is true when it is non-zero. Comparisons and logic are expression operators, not operations of their own. |

### Flow

| Operation | Operands | Output | What it does |
| --- | --- | --- | --- |
| If | 1. expression | - | Runs the lines below when the condition holds, and skips the block when it does not. |
| While | 1. expression | - | Repeats the lines below while the condition holds. |
| End block | - | - | Closes the nearest open `If` or `While`. |
| Jump | 1. line target | - | Continues at the target line. |
| Call | 1. line target | - | Runs the lines from the target as a function body and returns to the line after the `Call`. Calls nest up to 8 deep. |
| Return | - | - | Leaves the function body; in the main program it finishes the script. |
| Halt | - | - | Finishes the script. |

The editor assembles a `Call`: the target is a local body in the same file or a function linked from
a library file at load.

### Time

| Operation | Operands | Output | What it does |
| --- | --- | --- | --- |
| Delay | 1. milliseconds | - | Waits for the given time, with the script in `Waiting`. A negative delay counts as zero. |
| Wait until | 1. expression | - | Holds the script in `Waiting`, testing the condition again on each pass, until the condition holds. |
| Get time | - | Destination: an integer variable or output (`Index` or `Uint32`) | The device time in whole milliseconds. A `Number` destination cannot hold the count and is refused. |

### Service

| Operation | Operands | Output | What it does |
| --- | --- | --- | --- |
| Log | 1. code, 2. to 4. ignored | - | Writes a custom log entry whose code is the first operand's value, from the script service and tagged with the script's slot. The editor accepts up to three further operands, which the device ignores. |
| Register read | 1. block address | Destination: the field's value, in the field's own type | Reads a block field on this device. |
| Register write | 1. block address, 2. value | - | Writes the value into the block field, with a different TrID per instance. |
| Nop | - | - | Does nothing; a placeholder line. |
| Register read (foreign) | 1. device address, 2. block address | Destination: the field's value, in the field's own type | Reads a block field on another device and waits for the reply. |
| Register write (foreign) | 1. device address, 2. block address, 3. value | - | Writes a block field on another device and waits for the reply. |
| Load script | 1. file id, 2. slot | Destination: a result code, 0 on success | Loads a stored script file into a slot, as the Scripts page does. A slot that is occupied or out of range gives a non-zero code, and the running script cannot load over itself. |
| Unload script | 1. slot | Destination: a result code, 0 on success | Unloads a loaded script. A slot that holds no script or is out of range gives a non-zero code, and the running script cannot unload itself. |
| Start | - | - | Starts the script: entering `Running` resets the instruction counter and the call depth, so a restart begins at the first line. |
| Stop | - | - | Stops the script. |
| Pause | - | - | Pauses the script; a resume continues at the line after the `Pause`. |
| Resume | - | - | Continues a paused script. |
| Error | 1. code | - | Throws a script-defined error: the code is logged and the script halts in `Error`. |
| Report info | - | - | Logs the script's state and carries on. |

The state operations are separate instructions rather than values of one state instruction: a state
is a condition, while start, stop, pause and resume are actions on those conditions. A script that
runs one of them acts on itself; they are not a remote control for another slot.

### Compose

| Operation | Operands | Output | What it does |
| --- | --- | --- | --- |
| Compose | 1. index, 2. value, up to three more values | Destination: a variable or output container | Writes the values into the container's elements starting at the index. The container is a `Vector`, `Matrix`, `Colour` or `Text`, and each value converts to the element's type: a `Number` element, a `Uint32` byte of a `Colour`, a `Char` of a `Text`. |
| Extract | 1. container, 2. index | Destination: one element | Reads one element back in the container's element type. |

An index outside the container's bounds is an error on the line.

## Expressions

An expression is the operand stream of `Set`, `If`, `While` and `Wait until`: values interleaved with
operators and parentheses, read left to right.

| Operator | Form | Note |
| --- | --- | --- |
| Add (`+`), Subtract (`-`), Multiply (`*`), Divide (`/`), Modulo (`%`), Power (`^`) | Between two values | Arithmetic, element-wise, a scalar broadcasts. Division by zero fails, Modulo takes whole numbers, and Power takes an exponent whose integer part is at most 64. |
| Subtract (`-`) | Before a value | Negation. |
| AND, OR, XOR | Between two values | Logic, scalar operands only, result `Bool`. |
| NOT | Before a value | Logic, one scalar operand, result `Bool`. |
| Compare = (`=`), Compare != (`!=`), Compare < (`<`), Compare <= (`<=`), Compare > (`>`), Compare >= (`>=`) | Between two values | Comparison, scalar operands only, result `Bool`. |
| Open parenthesis (`(`), Close parenthesis (`)`) | Around a group | Groups the expression. |
| dot, cross | Prefix, two values | Vector dot product and vector cross product; `dot` gives a `Number`, `cross` a `Vector3`. |
| size, transpose | Prefix, one value | `size` is a vector norm, a `Number`; `transpose` swaps a matrix's rows and columns, up to 9 elements. |

The names are the picker's labels; the symbol after each is the operator it stands for. Precedence,
tightest first: Power; then Multiply, Divide and Modulo; then Add and Subtract; then the ordering
comparisons (<, <=, >, >=); then Compare = and Compare !=; then AND; then XOR; then OR. Parentheses
override it, and Power associates right to left while the rest associate left to right.

No operand and no intermediate result holds more than 9 elements, so a `Vector` of up to 9 or a
`Matrix` of up to 3 by 3 fits, and nesting is at most 8 levels deep, counting parentheses, prefix
operators and Power steps. A condition holds when its result is a non-zero scalar, and the editor
expects a `Bool`.

## Block Addressing

A register read or write names its target with a block address: a `BlockInfo` constant, created in
the block picker with New BlockInfo — choose the block, then the field, then the key. The picker
reads the choices from the device, so the field and key offered are the ones the block type
declares. A block is a type plus an instance, the instance selecting one of that type's 64
addresses.

| Part | Size | Range | What it is |
| --- | --- | --- | --- |
| Type | 10bit | 0 to 1023 | The block type, chosen by name |
| Instance | 6bit | 0 to 63 | Which block of that type |
| Field | 8bit | 0 to 255 | The field within the block, chosen by name |
| Key | 8bit | 0 to 255 | The key within the field, 0 where the field has no keys |

A script's own I/O is addressed the same way. Four block types of 64 instances each carry script
slots, and at those the block address names a script's input or output, so a block elsewhere on the
device reads or writes a script input or output through its block address.

Addressing is positional: there is no name binding and no file versioning, so a script file is
portable in the weak sense only, and the device's software version is the compatibility signal. A
block that is not present is an error raised by the read or write line when it runs, not a load
failure.

## Errors

A fault stops the script in `Error` and names the line that faulted. The editor reads the state and
the line back from the device and reports them with the operation that faulted.

| Rule | Meaning |
| --- | --- |
| Code 0 | No error, for a fault and for a result alike, so every operation's codes start at 1. |
| Instruction-specific | A code means what the operation that faulted defines it to mean. There is no shared table across operations. |
| Result or fault | One code space covers both. An operation that carries a result code into a destination reports success as 0; a failure the script does not check becomes an error. |

`Load script` and `Unload script` carry their result code into their destination instead of faulting.
`Get time` and Power do the opposite: a bad type or a bad domain is a fault, because it is a fault in
the program rather than a condition to handle.

## Common Patterns

Lines are written as the specification writes them: `In3`, `Out2`, `Var1`, `Const0`, `Ins<Name>` for
an instruction, `Op<Operator>` for an operator inside an expression, `EndLine` for the end of the
line. The editor shows the same line with each value's name and the instruction's category.

A gate — pass an input through while an enable is true, otherwise hold a fixed value. `Select` needs
no block and keeps no state:

    Out0, InsSelect, In1, In0, Const0, EndLine

An edge latch — fire once on a rising edge, by comparing the input with the level it last had:

    Out0, InsSet, In0, OpAnd, OpNot, Var0, EndLine
    Var0, InsSet, In0, EndLine

`Var0` holds the level the input had when those lines last ran, so the first line is true only on a
change from false to true: `Out0` is the input AND NOT the stored level. The second line stores the
level for the next pass. `NOT` is a prefix operator over its single scalar operand.

`Select` is the choice where one of two values is wanted; `If` with `End block` is the choice where
several lines share a condition.
