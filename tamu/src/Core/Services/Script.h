#pragma once

// Script manager service (Docs/Services/Script.md): CIDs 0-17 and the 64+ write-stream
// CIDs. The manager is the user-facing service (Status, Edits, IO); the instruction
// service is internal (scripts talk to other services through the VM's memory ops).
// The runtime instance pool and the main-loop scheduler live here; the VM itself is in
// Core/Functions/Script.h.

#include "Core/Functions/Script.h"

#define MAX_SCRIPT_INSTANCES 4

// One definition of the instance pool (included once via Dispatcher.h on cores that
// compile scripts in - nodes without USE_SCRIPTS never reference it).
static ScriptInstance script_instances[MAX_SCRIPT_INSTANCES];

// Write script (CID 16) streaming context. The script ID arrives in fragment 0 only,
// so later fragments are associated through this single in-flight record: the app
// writes sequentially (each fragment acknowledged before the next), which makes the
// single-context model safe. Fragment 0 of a new stream replaces the context.
static bool s_write_script_active = false;
static uint8_t s_write_script_id = 0;
static uint16_t s_write_script_seq = 0; // last contiguous fragment index written

// ---------------------------------------------------------------------------
// Instance lifecycle
// ---------------------------------------------------------------------------

static ScriptInstance *ScriptFindInstance(uint8_t id)
{
    for (int i = 0; i < MAX_SCRIPT_INSTANCES; i++)
        if (script_instances[i].script_id == id)
            return &script_instances[i];
    return nullptr;
}

// Frees all heap owned by an instance and resets the slot.
static void ScriptFreeInstance(ScriptInstance &inst)
{
    inst.program.Release();
    inst.runtime.Release();
    inst.stack.store.Release();
    for (int i = 0; i < MAX_MACRO_DEPTH; i++)
    {
        inst.macro_stack[i].program.Release();
        inst.macro_stack[i].runtime.Release();
    }
    inst = ScriptInstance{};
}

// Returns a slot for `id`: the existing instance, a free slot, or a non-running instance
// that can be evicted. Null when every slot is busy running/waiting.
static ScriptInstance *ScriptAllocateInstance(uint8_t id)
{
    ScriptInstance *existing = ScriptFindInstance(id);
    if (existing) return existing;

    ScriptInstance *evictable = nullptr;
    for (int i = 0; i < MAX_SCRIPT_INSTANCES; i++)
    {
        if (script_instances[i].script_id == 0)
        {
            script_instances[i] = ScriptInstance{};
            script_instances[i].script_id = id;
            return &script_instances[i];
        }
        if (script_instances[i].state != SCRIPT_RUNNING && script_instances[i].state != SCRIPT_WAITING)
            evictable = &script_instances[i];
    }
    if (evictable)
    {
        ScriptFreeInstance(*evictable);
        evictable->script_id = id;
        return evictable;
    }
    return nullptr;
}

// Loads the script file into the instance (fresh program, runtime, stack).
static bool ScriptLoadInto(ScriptInstance &inst)
{
    inst.program.Release();
    inst.runtime.Release();
    inst.stack.store.Release();
    for (int i = 0; i < MAX_MACRO_DEPTH; i++)
    {
        inst.macro_stack[i].program.Release();
        inst.macro_stack[i].runtime.Release();
    }
    inst.macro_depth = 0;
    inst.stack.used = 0;

    if (!LoadScriptProgram(inst.script_id, inst.program)) return false;
    if (!ValidateScriptProgram(inst.program)) { inst.program.Release(); return false; }
    if (!RuntimeSeed(inst)) { inst.program.Release(); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Reusable operations (used by the service handler and the CLI)
// ---------------------------------------------------------------------------

// Starts (or restarts) a script from line 0. Returns true on success.
static bool ScriptStart(uint8_t id)
{
    ScriptInstance *inst = ScriptAllocateInstance(id);
    if (!inst) return false;
    if (!ScriptLoadInto(*inst)) return false;
    inst->counter = 0;
    inst->tick_start = 0;
    inst->input_wake = false;
    inst->state = SCRIPT_RUNNING;
    return true;
}

// Pauses a running/waiting script; true if it was active.
static bool ScriptPause(uint8_t id)
{
    ScriptInstance *inst = ScriptFindInstance(id);
    if (!inst || (inst->state != SCRIPT_RUNNING && inst->state != SCRIPT_WAITING)) return false;
    inst->state = SCRIPT_PAUSED;
    return true;
}

// Resumes a paused script.
static bool ScriptResume(uint8_t id)
{
    ScriptInstance *inst = ScriptFindInstance(id);
    if (!inst || inst->state != SCRIPT_PAUSED) return false;
    inst->state = SCRIPT_RUNNING;
    inst->tick_start = 0;
    return true;
}

// Terminates a script (frees its instance). True when an instance existed.
static bool ScriptTerminate(uint8_t id)
{
    ScriptInstance *inst = ScriptFindInstance(id);
    if (!inst) return false;
    ScriptFreeInstance(*inst);
    return true;
}

// Returns the state byte of a script (Stopped when not loaded).
static uint8_t ScriptState(uint8_t id)
{
    ScriptInstance *inst = ScriptFindInstance(id);
    return inst ? inst->state : SCRIPT_STOPPED;
}

// Reads a script file's fixed header (32 bytes).
static bool ScriptReadHeader(uint8_t id, ScriptFileHeader &h)
{
    char fname[8];
    ScriptFileIdToName(id, fname);
    uint32_t off, sz;
    if (!Storage.GetFileInfo(fname, &off, &sz) || sz < ScriptHeaderSize()) return false;
    uint8_t hdr[ScriptHeaderSize()];
    if (Storage_FlashRead(off, hdr, ScriptHeaderSize()) != ScriptHeaderSize()) return false;
    return ScriptHeaderRead(hdr, ScriptHeaderSize(), &h);
}

// Counts the instruction lines of a script file by scanning its instruction section.
static uint16_t ScriptCountLines(uint8_t id)
{
    ScriptFileHeader h;
    if (!ScriptReadHeader(id, h)) return 0;
    char fname[8];
    ScriptFileIdToName(id, fname);
    uint32_t off, sz;
    if (!Storage.GetFileInfo(fname, &off, &sz)) return 0;
    uint32_t instr_off = 32 + (uint32_t)h.input_meta_len + h.input_value_len +
                         (uint32_t)h.output_count * 16 + (uint32_t)h.variable_count * 16 +
                         (uint32_t)h.constant_count * 4 + h.constant_values_len;
    uint32_t read_len = h.instruction_len;
    if (instr_off + read_len > sz) read_len = sz - instr_off;

    uint16_t lines = 0;
    uint32_t cursor = 0;
    uint8_t buf[256];
    while (cursor < read_len)
    {
        uint32_t chunk = read_len - cursor;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        Storage_FlashRead(off + instr_off + cursor, buf, chunk);
        for (uint32_t i = 0; i < chunk; i += 4)
            if (buf[i] == SYM_ENDLINE) lines++;
        cursor += chunk;
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Main-loop scheduler (Docs/Services/Script.md: run until loop-back or wait)
// ---------------------------------------------------------------------------

static void ScriptRun(ScriptInstance &inst)
{
    inst.tick_start = inst.counter;
    uint32_t budget = 512; // defensive cap: a pathological program cannot starve the loop
    while (budget-- > 0)
    {
        uint32_t line = inst.counter;
        if (ScriptExecLine(inst, line) == EXEC_STOP) return;
        if (inst.state != SCRIPT_RUNNING) return;

        // Advance past the line unless the instruction already moved the counter (jump).
        if (inst.counter == line) inst.counter = line + 1;
        if (inst.counter >= inst.program.line_count)
        {
            inst.state = SCRIPT_FINISHED; // fell off the end without an End symbol
            return;
        }
        if (inst.counter <= inst.tick_start) return; // loop boundary: continue next tick
    }
}

void ScriptTick()
{
    for (int i = 0; i < MAX_SCRIPT_INSTANCES; i++)
    {
        ScriptInstance &inst = script_instances[i];
        if (inst.script_id == 0) continue;
        if (inst.state == SCRIPT_WAITING)
        {
            if (inst.input_wake)
            {
                inst.input_wake = false;
                inst.state = SCRIPT_RUNNING;
            }
            else if ((int32_t)(Now() - inst.wake_time) >= 0)
            {
                inst.state = SCRIPT_RUNNING;
            }
            else
            {
                continue;
            }
        }
        if (inst.state == SCRIPT_RUNNING)
            ScriptRun(inst);
    }
}

// ---------------------------------------------------------------------------
// Service handler
// ---------------------------------------------------------------------------

void HandleScriptService(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    if (frame.flags & FLAG_TYPE) return; // Script service only processes requests

    PacketFrame reply;

    switch (cid)
    {
        case 0: // Get number of scripts
        {
            uint8_t n = ScriptFileCount();
            SendResponse(frame, &n, 1);
            break;
        }

        case 1: // Read Name (Script ID -> char[16])
        {
            if (PayloadBytes(frame) < 1) break;
            ScriptFileHeader h;
            if (!ScriptReadHeader(frame.payload[0], h))
            {
                RespondStatus(frame, false);
                break;
            }
            SendResponse(frame, (const uint8_t *)h.name, 16);
            break;
        }

        case 2: // Read I/O size (Script ID -> uint8 x2)
        {
            if (PayloadBytes(frame) < 1) break;
            ScriptFileHeader h;
            if (!ScriptReadHeader(frame.payload[0], h))
            {
                RespondStatus(frame, false);
                break;
            }
            uint8_t io[2] = {h.input_count, h.output_count};
            SendResponse(frame, io, 2);
            break;
        }

        case 3: // Read state
        {
            if (PayloadBytes(frame) < 1) break;
            uint8_t state = ScriptState(frame.payload[0]);
            SendResponse(frame, &state, 1);
            break;
        }

        case 4: // Set state (Script ID, new state)
        {
            if (PayloadBytes(frame) < 2) break;
            uint8_t id = frame.payload[0];
            uint8_t new_state = frame.payload[1];
            bool ok = false;
            switch (new_state)
            {
                case SCRIPT_RUNNING: // Start/restart, or resume when paused
                    ok = (ScriptState(id) == SCRIPT_PAUSED) ? ScriptResume(id) : ScriptStart(id);
                    break;
                case SCRIPT_PAUSED:
                    ok = ScriptPause(id);
                    break;
                case SCRIPT_STOPPED:
                    ScriptTerminate(id);
                    ok = true; // already stopped counts as success
                    break;
                default:
                    break; // Waiting/Finished/Error are internal-only
            }
            RespondStatus(frame, ok);
            break;
        }

        case 5: // Read input (Script ID, input index -> BlockMeta + value)
        {
            if (PayloadBytes(frame) < 2) break;
            uint8_t id = frame.payload[0];
            uint8_t idx = frame.payload[1];
            ScriptInstance *inst = ScriptFindInstance(id);
            BlockMeta meta;
            const uint8_t *val = nullptr;
            uint16_t vlen = 0;
            if (inst)
            {
                if (idx >= inst->program.header.input_count)
                {
                    RespondStatus(frame, false);
                    break;
                }
                FieldResult f = inst->runtime.Get(idx);
                if (!f.Data) { RespondStatus(frame, false); break; }
                meta = f.Descriptor; val = (const uint8_t *)f.Data; vlen = f.Descriptor.Size;
            }
            else
            {
                // Not loaded: report the file's default value for the input.
                ScriptProgram tmp;
                if (!LoadScriptProgram(id, tmp) || idx >= tmp.header.input_count)
                {
                    RespondStatus(frame, false);
                    break;
                }
                uint32_t step = tmp.input_meta_new ? 5 : 4;
                memcpy(&meta, tmp.input_meta + 4 + (uint32_t)idx * step, 4);
                uint32_t off = 0;
                for (uint8_t i = 0; i < idx; i++)
                {
                    BlockMeta m;
                    memcpy(&m, tmp.input_meta + 4 + (uint32_t)i * step, 4);
                    off += AlignTo4(m.Size);
                }
                vlen = meta.Size;
                val = tmp.input_values + off;
                uint8_t payload[MAX_PAYLOAD_SIZE];
                if (4 + vlen > sizeof(payload)) { tmp.Release(); RespondStatus(frame, false); break; }
                memcpy(payload, &meta, 4);
                memcpy(payload + 4, val, vlen);
                SendResponse(frame, payload, 4 + vlen);
                tmp.Release();
                break;
            }
uint8_t payload[MAX_PAYLOAD_SIZE];
            if (4 + vlen > sizeof(payload)) { RespondStatus(frame, false); break; }
            memcpy(payload, &meta, 4);
            if (vlen) memcpy(payload + 4, val, vlen);
            SendResponse(frame, payload, 4 + vlen);
            break;
        }

        case 6: // Write input (Script ID, input index, padding, BlockMeta, value)
        {
            if (PayloadBytes(frame) < 2 + 4) break;
            uint8_t id = frame.payload[0];
            uint8_t idx = frame.payload[1];
            const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(frame.payload + 2);
            const uint8_t *value = frame.payload + 2 + sizeof(BlockMeta);
            uint16_t value_len = desc->Size;
            uint16_t avail = PayloadBytes(frame) - 2 - sizeof(BlockMeta);
            if (value_len > avail) value_len = avail;

            ScriptInstance *inst = ScriptFindInstance(id);
            if (!inst)
            {
                // Writing an input of a stopped script creates a loaded instance so the
                // value survives until the script is started.
                inst = ScriptAllocateInstance(id);
                if (!inst || !ScriptLoadInto(*inst)) { RespondStatus(frame, false); break; }
                inst->state = SCRIPT_STOPPED;
            }
            if (idx >= inst->program.header.input_count)
            {
                RespondStatus(frame, false);
                break;
            }
            if (!inst->runtime.Set(idx, value, value_len, desc->FlagsAndType))
            {
                RespondStatus(frame, false);
                break;
            }
            inst->input_wake = true; // wake a Waiting script on external input
            RespondStatus(frame, true);
            break;
        }

        case 7: // Read output (Script ID, output index -> BlockMeta + value)
        {
            if (PayloadBytes(frame) < 2) break;
            uint8_t id = frame.payload[0];
            uint8_t idx = frame.payload[1];
            ScriptInstance *inst = ScriptFindInstance(id);
            BlockMeta meta = {(uint16_t)DataType::None, 0, 0};
            const uint8_t *val = nullptr;
            uint16_t vlen = 0;
            if (inst)
            {
                if (idx >= inst->program.header.output_count)
                {
                    RespondStatus(frame, false);
                    break;
                }
                FieldResult f = inst->runtime.Get((uint16_t)(inst->program.header.input_count + idx));
                if (!f.Data) { RespondStatus(frame, false); break; }
                meta = f.Descriptor; val = (const uint8_t *)f.Data; vlen = f.Descriptor.Size;
            }
            uint8_t payload[MAX_PAYLOAD_SIZE];
            if (4 + vlen > sizeof(payload)) { RespondStatus(frame, false); break; }
            memcpy(payload, &meta, 4);
            if (vlen) memcpy(payload + 4, val, vlen);
            SendResponse(frame, payload, (uint8_t)(4 + vlen));
            break;
        }

        case 8: // Get info (Script ID -> variable count u8, instruction count u16)
        {
            if (PayloadBytes(frame) < 1) break;
            uint8_t id = frame.payload[0];
            ScriptFileHeader h;
            if (!ScriptReadHeader(id, h))
            {
                RespondStatus(frame, false);
                break;
            }
            uint16_t lines = ScriptCountLines(id);
            uint8_t payload[3] = {h.variable_count, (uint8_t)(lines & 0xFF), (uint8_t)(lines >> 8)};
            SendResponse(frame, payload, 3);
            break;
        }

        case 9: // Read Variable (Script ID, Variable ID -> BlockMeta + value)
        {
            if (PayloadBytes(frame) < 2) break;
            uint8_t id = frame.payload[0];
            uint8_t idx = frame.payload[1];
            ScriptInstance *inst = ScriptFindInstance(id);
            BlockMeta meta = {(uint16_t)DataType::None, 0, 0};
            const uint8_t *val = nullptr;
            uint16_t vlen = 0;
            if (inst)
            {
                if (idx >= inst->program.header.variable_count)
                {
                    RespondStatus(frame, false);
                    break;
                }
                FieldResult f = inst->runtime.Get(
                    (uint16_t)(inst->program.header.input_count + inst->program.header.output_count + idx));
                if (!f.Data) { RespondStatus(frame, false); break; }
                meta = f.Descriptor; val = (const uint8_t *)f.Data; vlen = f.Descriptor.Size;
            }
            uint8_t payload[MAX_PAYLOAD_SIZE];
            if (4 + vlen > sizeof(payload)) { RespondStatus(frame, false); break; }
            memcpy(payload, &meta, 4);
            if (vlen) memcpy(payload + 4, val, vlen);
            SendResponse(frame, payload, 4 + vlen);
            break;
        }

        case 10: // Write Variable (Script ID, Variable ID, BlockMeta, value)
        {
            if (PayloadBytes(frame) < 2 + 4) break;
            uint8_t id = frame.payload[0];
            uint8_t idx = frame.payload[1];
            const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(frame.payload + 2);
            const uint8_t *value = frame.payload + 2 + sizeof(BlockMeta);
            uint16_t value_len = desc->Size;
            uint16_t avail = PayloadBytes(frame) - 2 - sizeof(BlockMeta);
            if (value_len > avail) value_len = avail;

            ScriptInstance *inst = ScriptFindInstance(id);
            if (!inst || idx >= inst->program.header.variable_count)
            {
                RespondStatus(frame, false);
                break;
            }
            uint16_t field = (uint16_t)(inst->program.header.input_count + inst->program.header.output_count + idx);
            if (!inst->runtime.Set(field, value, value_len, desc->FlagsAndType))
            {
                RespondStatus(frame, false);
                break;
            }
            RespondStatus(frame, true);
            break;
        }

        case 11: // Get current instruction (Script ID -> u16 line number)
        {
            if (PayloadBytes(frame) < 1) break;
            ScriptInstance *inst = ScriptFindInstance(frame.payload[0]);
            uint32_t counter = inst ? inst->counter : 0;
            uint8_t payload[2] = {(uint8_t)(counter & 0xFF), (uint8_t)(counter >> 8)};
            SendResponse(frame, payload, 2);
            break;
        }

        case 12: // Move to instruction (Script ID, instruction number)
        {
            if (PayloadBytes(frame) < 3) break;
            uint8_t id = frame.payload[0];
            uint32_t target = (uint32_t)(frame.payload[1] | (frame.payload[2] << 8));
            ScriptInstance *inst = ScriptFindInstance(id);
            if (!inst || target >= inst->program.line_count)
            {
                RespondStatus(frame, false);
                break;
            }
            inst->counter = target;
            inst->tick_start = 0;
            RespondStatus(frame, true);
            break;
        }

        case 13: // Create script (Script ID, 0xFF = auto-assign lowest free)
        {
            if (PayloadBytes(frame) < 1) break;
            uint8_t requested = frame.payload[0];
            uint8_t id = requested;
            if (id == 0xFF)
            {
                id = 0;
                for (uint16_t i = 1; i <= 255; i++)
                {
                    char fname[8];
                    ScriptFileIdToName(i, fname);
                    if (Storage.FileExists(fname) == 0xFFFFFFFF) { id = i; break; }
                }
                if (id == 0) { RespondStatus(frame, false); break; }
            }
            else if (id == 0)
            {
                RespondStatus(frame, false);
                break;
            }
            else
            {
                char fname[8];
                ScriptFileIdToName(id, fname);
                if (Storage.FileExists(fname) != 0xFFFFFFFF) { RespondStatus(frame, false); break; }
            }
            // The actual file is created by the write stream (CID 16) on its first
            // fragment; the ID is reserved here so the app gets a stable handle.
            SendResponse(frame, &id, 1);
            break;
        }

        case 14: // Delete script (Script ID)
        {
            if (PayloadBytes(frame) < 1) break;
            uint8_t id = frame.payload[0];
            ScriptTerminate(id);
            char fname[8];
            ScriptFileIdToName(id, fname);
            Storage.DeleteFile(fname);
            RespondStatus(frame, true);
            break;
        }

        case 15: // Read script (Script ID -> whole file as a FRAG stream)
        {
            // Response stream: fragment 0 = [frag info][script id (1)][contents], later
            // fragments = [frag info][contents]. The app knows the script file size
            // (from the storage file table) and trims the last fragment's wire padding.
            if (PayloadBytes(frame) < 1) break;
            uint8_t id = frame.payload[0];
            char fname[8];
            ScriptFileIdToName(id, fname);
            uint32_t off, sz;
            if (!Storage.GetFileInfo(fname, &off, &sz))
            {
                RespondStatus(frame, false);
                break;
            }
            uint32_t total_content = sz;
            uint16_t total_frags = (uint16_t)((total_content + 255) / 256);
            if (total_frags == 0) total_frags = 1; // empty file: single fragment

            uint8_t buf[MAX_PAYLOAD_SIZE];
            for (uint16_t f = 0; f < total_frags; f++)
            {
                uint8_t flags = FLAG_TYPE | FLAG_FRAG;
                if (f == 0) flags |= FLAG_START;
                if (f == total_frags - 1) flags |= FLAG_STOP;
                WriteFragInfo(buf, f, total_frags);
                uint16_t head = (f == 0) ? 1 : 0;
                if (head) buf[4] = id;
                uint32_t content_off = (uint32_t)f * 256;
                uint16_t content_len = (total_content - content_off > 256)
                                           ? 256
                                           : (uint16_t)(total_content - content_off);
                if (content_len)
                    Storage_FlashRead(off + content_off, buf + 4 + head, content_len);
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 flags, buf, 4 + head + content_len);
                DispatchPacket(reply);
            }
            break;
        }

        case 16: // Write script (stream: Script ID, Fragmentation, Contents)
        {
            // Request stream: fragment 0 = [frag info][script id (1)][contents], later
            // fragments = [frag info][contents]. The whole script is rewritten on each
            // save: fragment 0 deletes any existing file and creates it at total*256,
            // contents are written at offset current*256, and the last fragment shrinks
            // the file to the real (4-byte padded) content length. Each acknowledged
            // fragment is answered with the last sequential fragmentation index written.
            if (!(frame.flags & FLAG_FRAG)) break;
            PacketFragInfo frag = PacketGetFrag(frame);
            uint16_t plen = PayloadBytes(frame);

            uint8_t id;
            const uint8_t *contents;
            uint16_t content_len;
            if (frag.current == 0)
            {
                if (plen < 5) break; // [frag info (4)][script id (1)]
                id = frame.payload[4];
                contents = frame.payload + 5;
                content_len = plen - 5;
                // (Re)start the write context.
                char fname[8];
                ScriptFileIdToName(id, fname);
                if (Storage.FileExists(fname) != 0xFFFFFFFF)
                    Storage.DeleteFile(fname);
                uint32_t reserved = (uint32_t)frag.total * 256;
                if (!Storage.CreateFile(fname, reserved))
                {
                    DeviceLog("SCRIPT", "write script %u: create %lu B failed", (unsigned)id,
                              (unsigned long)reserved);
                    s_write_script_active = false;
                    s_write_script_seq = 0xFFFF;
                    break; // cannot continue the stream
                }
                s_write_script_active = true;
                s_write_script_id = id;
                s_write_script_seq = 0xFFFF; // so current == seq + 1 holds for fragment 0
            }
            else
            {
                if (!s_write_script_active) break; // fragment 0 never arrived
                id = s_write_script_id;
                contents = frame.payload + 4;
                content_len = plen - 4;
            }

            char fname[8];
            ScriptFileIdToName(id, fname);
            uint32_t file_offset, file_size;
            if (Storage.GetFileInfo(fname, &file_offset, &file_size))
            {
                // Write contiguously: a resend of the last index is idempotent, anything
                // ahead of it is a gap (respond with the last written index unchanged).
                if (frag.current == (uint16_t)(s_write_script_seq + 1))
                {
                    uint32_t write_off = (uint32_t)frag.current * 256;
                    uint32_t room = (write_off < file_size) ? (file_size - write_off) : 0;
                    uint32_t chunk = content_len;
                    if (chunk > room) chunk = room;
                    bool ok = (chunk == 0) ||
                              Storage_FlashWrite(file_offset + write_off, contents, chunk);
                    if (ok)
                    {
                        s_write_script_seq = frag.current;
                        // Last fragment: shrink the file to the real content length
                        // (the last fragment's wire content is padded to 4 bytes).
                        if (frag.total > 0 && frag.current == frag.total - 1)
                        {
                            uint32_t real_size = (uint32_t)(frag.total - 1) * 256 + content_len;
                            if (real_size > file_size) real_size = file_size;
                            Storage.ResizeFile(fname, real_size);
                        }
                    }
                    else
                    {
                        DeviceLog("SCRIPT", "write script %u flash write failed", (unsigned)id);
                    }
                }
            }

            if (frame.flags & FLAG_REQACK)
            {
                uint8_t ack[2] = {(uint8_t)(s_write_script_seq & 0xFF),
                                  (uint8_t)(s_write_script_seq >> 8)};
                SendResponse(frame, ack, 2);
            }
            break;
        }

        default:
            break;
    }
}