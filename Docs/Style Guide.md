Rules for the documents under `Docs/`: how they are structured, worded, formatted and linked. Follow
them when writing new documentation and when touching existing files. `Docs/Plan.md` is the owner's
personal notes and is out of scope.
### Scope
Applies to every document under `Docs/`: protocol, service and module documentation alike. The one
exception is `Plan.md`. When this guide and an existing document disagree, the guide wins and the
document is corrected.
### Document Anatomy
- The file name is the document's title. Do not repeat it as a heading, and use Title Case for folder
  and file names (`Services/System Block and Device Commands.md`).
- Open with an unnamed preamble: a brief description of what the thing is, plus any physical
  constraint that shapes it. Two or three sentences, never a paragraph of context.
- Then the body, in this order: structures, behaviour, commands, implementation functions.
- If the service or feature is gated by a compile-time define, carve out a small section at the very
  beginning naming it ("Use define `USE_SUB_REQUEST`.") and put nothing else in it.
- Mark unfinished work `TODO`, `WIP` or `TBD`. Nothing else stands in for incomplete content.
### Headings
- Top-level sections use `###`, sub-sections use `####`. `#` and `##` are never used, because the file
  name is the title.
- Title Case: capitalise the principal words and lowercase articles, conjunctions and short
  prepositions (a, an, the, and, or, of, for, in, on, to) unless they are the first or last word.
  "File System", "Trigger Types", "Commands for Cores (001x)".
- No heading ends in a colon. Its content follows it directly, with no blank line above or below.
- A blank line is needed only between two paragraphs, before a table that follows a paragraph,
  and between two consecutive tables. Nowhere else.
- Command sections are named by their ID range: `### Commands (030x)`, `### Requester Commands (041x)`.
  The leading digits are the high part of the CMD; the table's ID cell holds the low part, in hex.
### Voice
- Declarative and descriptive, in full sentences, kept short. "The provider sends an update when the
  timer expires", not "Sends when timer runs out".
- No modal verbs. State what is, not what should be. "Should be implemented per-device" becomes
  "Implemented per device, behind a common interface".
- Explain only where the reason is not obvious: one sentence, in place. Never open a background
  section. If a rule fits in a table row, make it a table row.
- Tables and lists carry structured information; prose carries the reasoning.
### Code and Symbols
- Backticks mark what must be typed exactly as written in code: types and struct names (`BlockInfo`,
  `Number`), identifiers meant as symbols (`BlockType`), functions (`FindFiletable`, `ProcessBus`),
  defines (`USE_DYNAMIC_BLOCKS`, `PAGE_SIZE`), file names and extensions (`.SV`, `.DT_XX`), literals
  (`0xAA`, `0xFFFFFFFF`), and full signatures.
- Do not backtick concepts or prose words: bit, block, page, fragmentation, file table, subscription.
  Nor units, section names, or service names used as prose: Storage, Register, Script.
- No fenced code blocks. Signatures and layouts are inline code or tables.
- Lists use `-` and nest with a tab.
### Types, Sizes and Numbers
- A defined custom type is written by its name in [[Data Formats]]: `Number`, `Index`, `Filename`,
  `BlockInfo`, `Vector`, `Matrix`, `Colour`, `Bool`, `Serial Number`, `Name`.
- Everything else follows C convention: `uint8`, `uint16`, `uint32`, `int32`, `char[8]`.
- Bit fields and flags use the `-bit` form: `10bit`, `6bit`, `8bit`.
- Templated types carry their arguments in angle brackets: `Vector<N>`, `Matrix<N,M>`. Every other
  array uses brackets: `ValueInfo[N1]`, `uint16[W x H]`, `uint8[32]`.
- Hex is uppercase and compact: `0x3F0-0x3F3`, `0x0000-0x0FFF`. No spaces around the dash.
- A space separates value and unit: `10 ms`, `120 s`, `460.8k baud`.
- Units belong in the table's Note column, never in the size cell.
### Tables
- One schema per use case, consistent within it. Tables of the same kind look the same; tables of
  different kinds may differ freely. Do not force an unrelated table into a schema.
- Command tables: `Name | ID | Request | Response | Note`.
- Block tables, for a block's addressable fields: `Name | F.K / F.K:SP | Flags | Size | Note`.
- Struct and payload tables, fields in order: `Name | Size | Note`.
- Tiered tables, for grouped structures: `Section | Part | Size | Note`. A Section contains Parts, so
  the grouping column comes first. Existing tables keep their contents: only the column names and
  their order change.
- Flag tables: `Flag | Description`, kept separate from the block table that uses the flags.
- Index and allocation tables are unrelated to each other and to the schemas above. Each keeps the
  shape that suits it; `Command ID table.md`, the TrID range table and the block-type table stay as
  they are.
- Column headers use Title Case: `Name | Key Name | Usual Type | Note`.
- A cell holding a sentence ends with a period. A fragment does not: `Core only`, `In bytes`, `ms`.
- `-` means absent, `...` means continues, an empty cell means nothing to note.
- The Note column carries behaviour and constraints as short sentences with the first word
  capitalised: "Respond only if requested", "Core only".
- Success cells carry the type: `Success (bool)`.
- Layout and wire sketches keep their own shape, which reads better left-to-right than forced into
  rows, but their size cells use the same notation as every other table.
### Links
- Use Obsidian wikilinks: `[[Services/Storage]]`, or `[[Services/Storage|Storage]]` when the display
  text must differ. They survive renames, which markdown paths do not.
- Link every mention, not only the first one in a file.
- Link documents, not symbols. Code symbols are backticked, never linked.
### Addresses and Acronyms
- The addressing notations are distinct and all canonical: `F.K:SP` for a field's position in a
  struct-position layout, `BlockInfo` for the packed 32-bit index, and `NetID.Device`: `3F.1`,
  `0.8`: for a device address. `BlockInfo` may also be written `T.I.F.K`, that is
  Type.Instance.Field.Key.
- Acronyms are defined once, globally, in [[General Architecture]], not per file.
- Case follows how each acronym is constructed: `TrID` (Transaction ID), `SNDB`, `CID`, `CRC8`,
  `NetID`, `RSBus`.
### Implementation Functions
- Every service documents its implementation functions, not only [[Services/Storage]].
- Group them by the service's own visibility classes, stating the class in the heading: "Main Functions
  (implement per device, ideally not exposed)", "Filesystem Utility Functions (filesystem only,
  universal)", "File Based Functions (accessible outside, universal)".
- Each entry is a backticked signature followed by a tab-indented description covering behaviour,
  parameters and return value.
### One Owner per Fact
- A service document owns its command names, payloads and behaviour. [[Command ID Table]] is an index
  only: section, range, command name: and links to the service document.
- [[Services/Register]] and the other service documents own protocol and firmware behaviour. The
  documents under `App/Service views/` own UI presentation and link to the service document for
  anything protocol-level rather than restating it.
- [[General Architecture]] owns the code rules. `AGENTS.md` keeps only agent-operational rules: git,
  gate commands, budgets: and points there.
### Spellings to Fix
- `recieved` to received, `avalible` to available, `prefferably` to preferably, `preffered` to
  preferred, `persistance` to persistence, `neccesarily` to necessarily, `accesible` to accessible,
  `adresses` to addresses, `optimalization` to optimisation, `whereever` to wherever, `comparision` to
  comparison, `Reciever` to Receiver, `eculidian` to euclidean, `selfdescribing` to self-describing,
  `entrire` to entire.
- `it's` is only "it is". The possessive is `its`.
- UK English throughout: colour, behaviour, optimisation. Correct the existing US forms such as
  `serialization` and `deserialization`.
