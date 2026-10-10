Rules for the source under `firmware/` and `app/`: how it is named, laid out, limited and kept small. [[General Architecture]] describes what the firmware is built from and points here; [[Style Guide]] covers the documents under `Docs/`.
### Scope
Applies to everything under `firmware/src/` and `app/lib/`. New and edited code follows it; it is not a licence to reformat files nobody is touching. Where this guide and an existing file disagree, the guide wins for the lines being touched - a wholesale reformat is its own change-set with its own size measurement.
### Services
- Each service or large feature is one class, declared in its top header (`Core/Services/Register.h`), with its methods defined across the service's split files.
- One instance per binary, defined in that top header: `RegisterService Register;`. A device enables a service by which `Handle` the dispatcher calls, not by allocating an instance.
- Non-virtual methods, no inheritance, no vtable, and no constructor that needs runtime work. `Init()` is explicit and called from the device's `Main()`, so static-initialisation order never matters; it resets the service's own state and does nothing else.
- A service's member state is named for what it holds, with no prefix: `WriteOffset`, `NetId`, `RegistryTable`.
- `Handle(const PacketFrame &Frame)` returns whether the service consumed the packet. The dispatcher is a table of `(service, handler)` entries rather than a switch.
- A function that belongs to a service is a member of it. Only the framing helpers (`Packet`, `Dispatcher`, `Crc8`, `TimeSync`) stay free functions.
### Naming
- PascalCase for every identifier the project names: types, classes, functions and methods, enum types and their members, class members, locals, parameters and globals: `PacketFrame`, `HandleRegister`, `BlockTypeRange`, `StorageBlockFS`, `WriteOffset`, `ContentLen`.
- No prefixes on state. A member or a static is named for what it holds, not for its storage class: `WriteOffset`, never `s_write_off` or `m_writeOffset`.
- Identifiers that belong to an external library keep their own spelling and are never converted: the NimBLE and ESP-IDF symbols, the CH32 device headers, and what they return.
- An acronym keeps its canonical spelling instead of being re-cased into PascalCase: `GetServiceCID`, `NextSystemTrID`, `SNDBFile`, `CRC8`. The canonical forms are listed in [[General Architecture]].
- Macros and compile-time defines are UPPER_SNAKE: `MAX_PAYLOAD_SIZE`, `USE_SCRIPTS`, `FIELD_RESPONSE_MAX_VALUE`.
- An enum class names the type and gives every member an explicit value: `enum class RegisterCid : uint8_t { EnumerateBlocks = 0, Read = 2 };`
- Files are named after the type or topic they hold, in PascalCase. A split service's parts are `Service` plus `Part`: `RegisterRead.h`, `SubscriptionsProvider.h`.
### Formatting
- Four spaces per level, spaces only; the tree has no tabs.
- An opening brace goes on its own line, including a case block.
- A one-line body omits the braces entirely: `if (IsResponse) return true;`, `while (Steps++ < Budget) if (Ready) Advance();`.
- Keep lines under 120 columns.
- One statement per line.
- Tables and register literals are laid out in columns, one row per line, so the shape reads: the `static constexpr` tables and the opcode maps.
- One blank line between functions; none between a comment and what it documents.
### Comments
- `//`, in sentences, above what they describe. Say why, not what.
- A comment earns its place by fixing a constraint: a wire layout, a unit, a documented rule, a measured number, or an ordering that is load-bearing.
- No commented-out code, no changelog comments, and no restating the next line.
### C++ Subset
- No `std::` containers, algorithms, `function`, exceptions or RTTI, and no feature that pulls in a vtable or a heap-allocating runtime. The one `std::string` in the tree is a value the BLE library returns; new code introduces none.
- No `float` and no `double`, not even the standard ones. Fixed point is `Number` (Q16.16) with the helpers in `Core/Types/Number.h`.
- `malloc`, `realloc` and `free`, never `new` and `delete`. A header-only service owns no heap and needs no allocator.
- `#pragma once` opens every header, and a header includes what it uses rather than relying on include order.
- Compile-time guards name a feature (`USE_SCRIPTS`, `USE_APP_INTERFACE`) or an ability (`SCALAR_ONLY`, `TYPE_CORE`). Device identity is a capability, never a `BOARD_*` ifdef.
- Anything crossing the wire or the flash is laid out to a known size and asserted with `static_assert`.
### Functions
- Around 20 lines, 50 at most, except a dispatcher switch whose arms only call functions.
- One purpose each, and an early return in preference to nested conditionals.
- A file-local helper is `static` and is defined before its first use.
### Memory
- Constant data is `static constexpr` so it stays in flash; only state that changes at runtime occupies RAM.
- A compile-time table beats a chain of conditionals, and a switch beats both when the cases differ in behaviour.
### App
- Dart follows `dart format`, the `flutter_lints` set and Effective Dart; `flutter analyze` stays clean.
- Protocol symbols mirror the firmware's spellings so the two read alike, and a server document's behaviour is never restated in the app.
