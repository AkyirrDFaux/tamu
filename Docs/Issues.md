# Issues - Rebuild Tamu v2.0A 2026-09-06 full strict docs (no legacy)

## Fixed per previous recheck:
- Packet 128B payload116 FRAG112 fixed Device/Log 256->112 (Device.h:57 LogHandler.h:118), Tamu RSBus 460800 DAS 460800 (DAS/RSBus.h:83), Trigger flag FC00 (Enums.h:27 Trigger 0x1000), ID 6+10 app types.dart:15, storage 112 frag, capability bits, DataType spec.

## Critical Architecture Issues (2026-09-06) - MOSTLY FIXED ✅

### 1. Wrong Services in Dispatcher ✅ FIXED
Per Command ID table & General architecture.md, ONLY 4 core services should exist:
- Device (0x00) - Discover, Ping, Identify, TimeSync, SNDB
- Register (0x01) - Enumerate, Read, Write, Save, Recall, Dynamic commands
- Log Handler (0x02) - Report, Read, Clear
- Storage (0x03) - Format, Create, Delete, Resize, Rename, Read, Write

**Removed from dispatcher (were incorrectly implemented as services):**
- SystemMemory (0x04) - NOT a service, static memory accessed via Register
- DynamicMemory (0x05) - NOT a service, accessed via Register (CID 0x10-0x14)
- KeyedMemory (0x06) - Folded into static/dynamic, NOT a service
- Script (0x08) - Not in Command ID table yet
- ScriptInstructions (0x09) - Not in Command ID table yet
- Router (0x10) - Router-only, not for core

### 2. Naming: "System Memory" → "Static Memory" ✅ FIXED
- Block type 0 in Register service = System Block (device info)
- Block types 3+ (LEDButton, PWM, AccGyr, Vysi1Display) = **Static Memory blocks**
- Renamed `SystemMemory.h` → `StaticMemory.h` (backup/restore service)
- Updated all references in code
- `Core/Functions/SystemMemory.h` → `Core/Functions/MemoryTypes.h` (shared types: BlockMeta, BlockSchema, StaticBlockDescriptor, FieldResult)

### 3. TRID Manager Integration ✅ FIXED
- `TridManager` class in `TrID.h` (renamed from `Trid.h`) with `GlobalTrid` instance
- Integrated into `DispatchPacket()` for response routing
- Responses (FLAG_TYPE) now routed through `GlobalTrid.HandleResponse()`

### 4. Register Service Handles All Memory Access ✅ FIXED (PARTIAL)
Per docs:
- Static Memory (LEDButton, PWM, AccGyr, Vysi1Display) → Register CID 010x
- Dynamic Memory (user-created blocks) → Register CID 010x + 011x (CID 0x10-0x14)
- Keyed Memory → Folded into static/dynamic, accessed via Register
- CLI updated to use Register service (0x01) instead of SystemMemory (0x04)

**PARTIAL: Dynamic/Keyed commands (CIDs 0x10-0x14) not fully working in firmware Register service**
- App clients (DynamicMemoryClient, KeyedMemoryClient) updated to use Register service
- Firmware Register service CID 0x10-0x14 handlers implemented but need registry management functions from removed DynamicMemory/KeyedMemory services

### 5. ServiceType Enum Updated ✅ FIXED
```cpp
enum class ServiceType : uint8_t
{
    Device = 0x00,
    Register = 0x01,
    LogHandler = 0x02,
    Storage = 0x03,
    Script = 0x08,
    ScriptInstructions = 0x09,
    App = 0x11,
    CLI = 0x12,
    Router = 0x10
};
```
Removed SystemMemory (0x04), DynamicMemory (0x05), KeyedMemory (0x06) from enum.

### 6. BlockLog Removal & Direct Memory Mirror Save/Recall ✅ FIXED
- Removed `BlockLog.h` (log-based backup replaced with direct 1:1 memory mirror per docs)
- Implemented direct 1:1 memory mirror save/recall per Register.md: "The storage is structurally 1:1 mirror of the memory. A new file is created, the old and new data merged into it, and old file is deleted."
- Updated Register service Save (01.03) / Recall (01.04) to use direct memory mirror
- Updated StaticMemory backup/restore to use direct mirror

### 7. KeyedMemory Files Removed ✅ FIXED
- Removed `Core/Services/KeyedMemory.h` (service removed)
- Kept `KeyedRegistry keyed_block_registry` definition in `StaticMemory.h` for Vysi1Display compatibility
- Updated Vysi1Display to include `Core/Functions/Memory.h` for keyed_block_registry access

### 8. DynamicMemory as Register Extension ✅ FIXED (PARTIAL)
- `Core/Services/DynamicMemory.h` kept for registry but removed from dispatcher
- Dynamic memory operations now via Register service CID 0x10-0x14 (NOT YET FULLY WORKING IN FIRMWARE)

### 9. App/CLI Updates ✅ FIXED
- CLI ParseService: only "r"/0x01 (Register) accepted
- CmdCreate: uses Register service for dynamic block creation (CID 0x10)
- CmdSave/CmdRecall/CmdReadMemory: default to Register service
- Removed DynamicMemory/KeyedMemory from CLI handlers

### 10. Capability Bits Updated ✅ FIXED
Removed `DynamicMemory` (1<<3) and `KeyedMemory` (1<<4) from Capabilities enum and Tamu_v2.0A kCapabilities.

## Verification Status
All **core** tests pass on hardware (ESP32-C3 Tamu v2.0A at `/dev/ttyACM1`) when run sequentially:
- `hardware_storage_test.dart` ✅
- `hardware_register_test.dart` ✅  
- `tamu_hardware_verification_test.dart` ✅

**Note: Tests must run sequentially (not in parallel) due to single serial port contention.**

Firmware compiles: RAM 7.9%, Flash 19.6%

## Known Issues / Remaining Work

### 1. Dynamic/Keyed Memory Commands Not Fully Implemented in Firmware Register Service
The Register service firmware doesn't yet handle CIDs 0x10-0x14 (Create Dynamic, Delete Dynamic, Get Name, Set Name, Get Memory Usage) because it needs the DynamicBlockDescriptor/KeyedBlockDescriptor registry management functions which were in the removed DynamicMemory/KeyedMemory services.

This causes:
- `keyed_refresh_test.dart` ❌
- `keyed_crash_test.dart` ❌  
- `dyn_flow_test.dart` ❌
- `mem_probe_test.dart` ❌
- `hil_script_test.dart` (MEM_WRITE test) ❌

**Required firmware work:** Move DynamicBlockDescriptor/KeyedBlockDescriptor registry management to a common Memory module accessible by Register.h, or implement CID 0x10-0x14 handlers in Register.h with proper registry access.

### 2. App Tests Needing Update (after firmware fix)
- `keyed_refresh_test.dart` - uses KeyedMemoryClient (service 0x06)
- `keyed_crash_test.dart` - uses KeyedMemoryClient (service 0x06)
- `dyn_flow_test.dart` - uses DynamicMemoryClient (service 0x05)
- `mem_probe_test.dart` - uses both KeyedMemoryClient and DynamicMemoryClient
- `hil_script_test.dart` - MEM_WRITE test creates dynamic block via Register service CID 0x10

### 3. Script Service Tests
- `hil_script_test.dart` - MEM_WRITE test creates dynamic block via Register service CID 0x10

## Previously Verified (working):
- firmware pio run -e Tamu_v2_0A SUCCESS RAM 7.9% Flash 19.6%
- app flutter analyze No issues
- Storage 03.0x, Register 01.0x (basic), Device 00.0x, Log 02.0x all working on hardware
- RSBus 460800, ID 6+10, payload 116, FRAG 112

## Remaining docs TODOs (stubs, space reserved):
- LED display dictionaries and keys (Modules and blocks/LED display.md:25 TODO)
- Router (Services/Router.md TODO) single-bus stub Router.h
- Subscriptions (Services/Subscriptions.md TODO 14 lines) stub Subscriptions.h
- Script (Services/Script.md TODO REDO) kept off per request, USE_SCRIPTS disabled
- Command ID table Subscriptions/Script empty, Router absent, System Memory vs Register alias
- Packet Data Formats.md:46 5+64 vs 29 units doc contradiction kept as 29 units (116) per code