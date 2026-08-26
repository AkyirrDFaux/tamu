#pragma once

// Script model (Docs/Services/Script.md). Each script is a storage file; the program is a
// stream of 4-byte symbols ([Type][Subtype][Value u16 LE]). The device service handler and
// the scheduler live in Core/Services/Script.h; this header holds the data model, the file
// parser and the virtual machine (no global state, so it is safe in any TU).
//
// v1 semantics (documented in Docs/Issues.md):
//  - A line is [Output][Instruction][Input...] EndLine. The input of a normal instruction
//    holds only operand symbols (Input/Output/Variable/Constant/Predefine) - function
//    outputs cannot feed another function directly (Docs/Services/Script.md). If/While
//    lines embed a math/logic expression that leaves one Bool on the stack.
//  - The primary instruction consumes the operand stack and stores its result to the
//    Output symbol (a Variable or Output). Flow terminators use degenerate lines
//    ([EndIf|EndWhile|End] EndLine).
//  - Execution runs forward until a branch targets a line at or behind the line where the
//    current tick started (loop boundary), the script waits (Delay / input wake), finishes
//    (End) or errors. One body iteration per main-loop tick.
//  - MemRead/MemWrite take an address from a Variable/Constant value: 4 bytes
//    [block][field][key][service] where service is the ServiceType of the target service.

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "Core/Functions/Memory.h"
#include "Core/Functions/Storage.h"
#include "Core/Functions/SysFunctions.h"
#include "Core/Functions/Log.h"
#include "Core/Types/Number.h"

// --- State bytes (Docs/Services/Script.md "State") ---
enum ScriptState : uint8_t
{
    SCRIPT_STOPPED = 0,
    SCRIPT_RUNNING = 1,
    SCRIPT_PAUSED = 2,
    SCRIPT_WAITING = 3,
    SCRIPT_FINISHED = 4,
    SCRIPT_ERROR = 5
};

// --- Symbol types (Docs/Services/Script.md "Symbol") ---
enum ScriptSymbolType : uint8_t
{
    SYM_INSTRUCTION = 0,
    SYM_INPUT = 1,
    SYM_OUTPUT = 2,
    SYM_VARIABLE = 3,
    SYM_CONSTANT = 4,
    SYM_ENDLINE = 5,
    SYM_PREDEFINE = 6
};

// --- Instruction subtypes (Docs/Services/Script.md "Functions") ---
enum ScriptOpcode : uint8_t
{
    OP_ADD = 0, OP_SUB, OP_MUL, OP_DIV, OP_NEG,
    OP_AND, OP_OR, OP_NOT,
    OP_CMP_EQ, OP_CMP_NE, OP_CMP_LT, OP_CMP_LE, OP_CMP_GT, OP_CMP_GE,
    OP_COMPOSE_VEC, OP_COMPOSE_COLOUR, OP_EXTRACT,
    OP_MEM_READ, OP_MEM_WRITE,
    OP_IF, OP_WHILE, OP_END_IF, OP_END_WHILE, OP_END,
    OP_DELAY, OP_GET_TIME,
    OP_PAUSE, OP_RESUME, OP_TERMINATE, OP_RESTART, OP_INFO_REPORT, OP_ERROR_HALT,
    OP_MACRO_CALL,
    OP_MAX
};

// --- Predefine subtypes (Docs/Services/Script.md "Symbol") ---
enum PredefineSubtype : uint8_t
{
    PRE_STATE = 0,
    PRE_TYPE = 1,
    PRE_INDEX = 2,
    PRE_CHAR = 3,
    PRE_MATHOP = 4,
    PRE_BOOL = 5
};

// A single program symbol: [Type u8][Subtype u8][Value u16 LE].
struct ScriptSymbol
{
    uint8_t type;
    uint8_t subtype;
    uint16_t value;
};

static inline void ScriptSymbolWrite(uint8_t *out, const ScriptSymbol &s)
{
    out[0] = s.type;
    out[1] = s.subtype;
    out[2] = (uint8_t)(s.value & 0xFF);
    out[3] = (uint8_t)(s.value >> 8);
}

static inline bool ScriptSymbolRead(const uint8_t *buf, uint32_t len, uint32_t offset, ScriptSymbol *out)
{
    if (offset + 4 > len) return false;
    out->type = buf[offset];
    out->subtype = buf[offset + 1];
    out->value = (uint16_t)(buf[offset + 2] | (buf[offset + 3] << 8));
    return true;
}

// Fixed 32-byte file header (Docs/Services/Script.md "File blocks").
struct ScriptFileHeader
{
    char name[16];
    uint8_t input_count;
    uint8_t output_count;
    uint8_t variable_count;
    uint8_t constant_count;
    uint16_t input_meta_len;
    uint16_t input_value_len;
    uint32_t constant_values_len;
    uint32_t instruction_len;
};

static inline bool ScriptHeaderRead(const uint8_t *buf, uint32_t len, ScriptFileHeader *h)
{
    if (len < 32) return false;
    memcpy(h->name, buf, 16);
    h->input_count = buf[16];
    h->output_count = buf[17];
    h->variable_count = buf[18];
    h->constant_count = buf[19];
    h->input_meta_len = (uint16_t)(buf[20] | (buf[21] << 8));
    h->input_value_len = (uint16_t)(buf[22] | (buf[23] << 8));
    h->constant_values_len = (uint32_t)(buf[24] | (buf[25] << 8) | (buf[26] << 16) | (buf[27] << 24));
    h->instruction_len = (uint32_t)(buf[28] | (buf[29] << 8) | (buf[30] << 16) | (buf[31] << 24));
    return true;
}

static inline constexpr uint32_t ScriptHeaderSize() { return 32; }

// Loaded script program: a RAM copy of the storage file's sections.
struct ScriptProgram
{
    ScriptFileHeader header;
    char file_name[8];      // storage name the file was read from
    uint8_t *input_meta;    // input_meta_len bytes: dict BlockMeta + per-input key BlockMeta
    uint8_t *input_values;  // input_value_len bytes: aligned default input values
    uint8_t *output_names;  // output_count * 16
    uint8_t *variable_names;// variable_count * 16
    uint8_t *constant_meta; // constant_count * 4
    uint8_t *constant_values; // constant_values_len bytes (aligned)
    uint8_t *instructions;  // instruction_len bytes; If/While/EndWhile Values patched in place
    uint16_t line_count;    // number of EndLine-terminated lines
    bool input_meta_new;    // input meta carries a style byte per input (5 B/input) vs legacy 4 B

    void Release()
    {
        free(input_meta); free(input_values);
        free(output_names); free(variable_names);
        free(constant_meta); free(constant_values);
        free(instructions);
        memset(this, 0, sizeof(*this));
    }
};

// Packs the storage name for a script id: "SCR" + 3-digit zero-padded id + 2 spaces.
static inline void ScriptFileIdToName(uint8_t id, char out[8])
{
    memset(out, ' ', 8);
    out[0] = 'S'; out[1] = 'C'; out[2] = 'R';
    out[3] = (char)('0' + (id / 100) % 10);
    out[4] = (char)('0' + (id / 10) % 10);
    out[5] = (char)('0' + id % 10);
}

// Returns the script id for an 8-byte storage name, or 0 if it is not a script file.
static inline uint8_t ScriptFileNameToId(const char name[8])
{
    if (name[0] != 'S' || name[1] != 'C' || name[2] != 'R') return 0;
    if (name[3] < '0' || name[3] > '9' || name[4] < '0' || name[4] > '9' ||
        name[5] < '0' || name[5] > '9') return 0;
    return (uint8_t)((name[3] - '0') * 100 + (name[4] - '0') * 10 + (name[5] - '0'));
}

// Counts script files in the storage file table (names matching the SCRnnn scheme).
static inline uint8_t ScriptFileCount()
{
    uint8_t count = 0;
    uint8_t n = Storage.FileCount();
    for (uint8_t i = 0; i < n; i++)
    {
        FileEntry e;
        if (!Storage.ReadFileEntry(i, &e)) break;
        if (ScriptFileNameToId(e.name) != 0) count++;
    }
    return count;
}

// Runtime value stack: a DynamicBlockDescriptor whose fields are pushed values.
struct ScriptValueStack
{
    DynamicBlockDescriptor store;
    uint16_t used = 0;
};

// A macro-call frame: the caller's program/runtime are moved aside while the macro runs.
#define MAX_MACRO_DEPTH 4
struct ScriptMacroFrame
{
    ScriptProgram program;
    DynamicBlockDescriptor runtime;
    uint32_t counter; // caller's return line
};

// One running (or loaded) script.
struct ScriptInstance
{
    uint8_t script_id = 0; // 0 = free slot
    uint8_t state = SCRIPT_STOPPED;
    uint32_t counter = 0;    // current line index
    uint32_t tick_start = 0; // first line executed this tick (loop-back boundary)
    uint32_t wake_time = 0;  // Now() at which a Delay wait expires
    bool input_wake = false; // a waiting script was woken by an input write
    ScriptProgram program;
    DynamicBlockDescriptor runtime; // inputs, then outputs, then variables
    ScriptValueStack stack;
    ScriptMacroFrame macro_stack[MAX_MACRO_DEPTH];
    uint8_t macro_depth = 0;
};

// ---------------------------------------------------------------------------
// Program loading / parsing
// ---------------------------------------------------------------------------

// Parses a raw file buffer into `p` (heap sections). Frees `p` on failure.
static bool ParseScriptBuffer(const uint8_t *buf, uint32_t sz, const char fname[8], ScriptProgram &p)
{
    memset(&p, 0, sizeof(p));
    memcpy(p.file_name, fname, 8);
    if (!ScriptHeaderRead(buf, sz, &p.header)) return false;

    const ScriptFileHeader &h = p.header;
    uint32_t in_meta = h.input_meta_len;
    uint32_t in_val = h.input_value_len;
    uint32_t out_names = (uint32_t)h.output_count * 16;
    uint32_t var_names = (uint32_t)h.variable_count * 16;
    uint32_t const_meta = (uint32_t)h.constant_count * 4;
    uint32_t const_val = h.constant_values_len;
    uint32_t instr = h.instruction_len;
    uint32_t total = 32 + in_meta + in_val + out_names + var_names + const_meta + const_val + instr;
    if (total > sz) return false;

    uint32_t cursor = 32;
#define SCRIPT_ALLOC_SECT(dst, len)                       \
    do {                                                  \
        uint32_t L = (len);                               \
        (dst) = (uint8_t *)malloc(L ? L : 1);             \
        if (!(dst)) { p.Release(); return false; }        \
        memcpy((dst), buf + cursor, L);                   \
        cursor += L;                                      \
    } while (0)
    SCRIPT_ALLOC_SECT(p.input_meta, in_meta);
    SCRIPT_ALLOC_SECT(p.input_values, in_val);
    SCRIPT_ALLOC_SECT(p.output_names, out_names);
    SCRIPT_ALLOC_SECT(p.variable_names, var_names);
    SCRIPT_ALLOC_SECT(p.constant_meta, const_meta);
    SCRIPT_ALLOC_SECT(p.constant_values, const_val);
    SCRIPT_ALLOC_SECT(p.instructions, instr);
#undef SCRIPT_ALLOC_SECT

    // Input meta must hold the dict BlockMeta + one key BlockMeta per input; the new
    // format adds an interaction-style byte per input (5 B/input, legacy is 4 B/input).
    if (h.input_count > 0)
    {
        uint32_t min_len = 4 + (uint32_t)h.input_count * 4;
        if (in_meta < min_len || in_meta > min_len + (uint32_t)h.input_count)
        {
            p.Release();
            return false;
        }
        p.input_meta_new = (in_meta == min_len + (uint32_t)h.input_count);
    }
    return true;
}

// Loads a script file from storage into `p`. Returns false on any failure.
static bool LoadScriptProgram(uint8_t id, ScriptProgram &p)
{
    char fname[8];
    ScriptFileIdToName(id, fname);
    uint32_t off, sz;
    if (!Storage.GetFileInfo(fname, &off, &sz) || sz < ScriptHeaderSize()) return false;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) return false;
    bool ok = (Storage_FlashRead(off, buf, sz) == sz) && ParseScriptBuffer(buf, sz, fname, p);
    free(buf);
    return ok;
}

// Symbol offset of `line` (walks the instruction stream counting EndLine symbols).
static uint32_t ScriptLineOffset(const ScriptProgram &p, uint32_t line)
{
    uint32_t offset = 0;
    uint32_t l = 0;
    while (offset + 4 <= p.header.instruction_len)
    {
        if (l == line) return offset;
        if (p.instructions[offset] == SYM_ENDLINE) l++;
        offset += 4;
    }
    return p.header.instruction_len;
}

// Validates the instruction stream (Docs/Services/Script.md validity rules) and patches the
// If/While/EndWhile symbol Values with jump targets. Returns the line count (0 = invalid).
static bool ValidateScriptProgram(ScriptProgram &p)
{
    struct FlowEntry
    {
        uint32_t sym_offset;
        uint16_t line_index;
        bool is_while;
    } flow_stack[64];
    uint8_t flow_depth = 0;

    uint32_t len = p.header.instruction_len;
    uint8_t *ins = p.instructions;
    uint32_t offset = 0;
    uint32_t lines = 0;
    bool has_end = false;

    auto IsConditionOp = [](uint8_t op)
    {
        return op <= OP_CMP_GE; // math + logic range
    };

    while (offset + 4 <= len)
    {
        uint32_t line_start = offset;
        ScriptSymbol syms[64];
        uint8_t sym_count = 0;
        for (;;)
        {
            if (sym_count >= 64 || offset + 4 > len) return false; // line too long / truncated
            ScriptSymbol sym;
            ScriptSymbolRead(ins, len, offset, &sym);
            syms[sym_count++] = sym;
            offset += 4;
            if (sym.type == SYM_ENDLINE) break;
        }
        if (syms[sym_count - 1].type != SYM_ENDLINE) return false;
        if (lines >= 0xFFFF) return false; // line_count must fit uint16
        uint16_t line_index = (uint16_t)lines;
        lines++;

        // Degenerate flow terminator: [EndIf|EndWhile|End] EndLine
        if (syms[0].type == SYM_INSTRUCTION)
        {
            uint8_t op = syms[0].subtype;
            if (op != OP_END_IF && op != OP_END_WHILE && op != OP_END) return false;
            if (sym_count != 2) return false;
            if (op == OP_END)
            {
                has_end = true;
                continue;
            }
            if (flow_depth == 0) return false;
            FlowEntry &e = flow_stack[flow_depth - 1];
            if ((op == OP_END_IF) == e.is_while) return false; // must match the same kind
            if (op == OP_END_IF)
            {
                ScriptSymbol patch = {SYM_INSTRUCTION, OP_IF, line_index}; // exit = after EndIf
                patch.subtype = e.is_while ? OP_WHILE : OP_IF;
                ScriptSymbolWrite(ins + e.sym_offset + 4, patch);
                flow_depth--;
            }
            else
            { // OP_END_WHILE: loop back to the While, exit = after EndWhile
                ScriptSymbol while_patch = {SYM_INSTRUCTION, OP_WHILE, line_index};
                ScriptSymbolWrite(ins + e.sym_offset + 4, while_patch);
                ScriptSymbol end_patch = {SYM_INSTRUCTION, OP_END_WHILE, e.line_index};
                ScriptSymbolWrite(ins + line_start, end_patch);
                flow_depth--;
            }
            continue;
        }

        // Normal line: [Output][Instruction][Input...]
        if (syms[0].type != SYM_OUTPUT && syms[0].type != SYM_VARIABLE) return false;
        uint16_t oi = syms[0].value;
        if (syms[0].type == SYM_OUTPUT && oi >= p.header.output_count) return false;
        if (syms[0].type == SYM_VARIABLE && oi >= p.header.variable_count) return false;
        if (sym_count < 3) return false; // need [out][op]...EndLine at minimum
        if (syms[1].type != SYM_INSTRUCTION) return false;
        uint8_t op = syms[1].subtype;
        if (op >= OP_MAX) return false;
        if (op == OP_END || op == OP_END_IF || op == OP_END_WHILE) return false; // must be degenerate

        bool is_flow = (op == OP_IF || op == OP_WHILE);
        if (is_flow)
        {
            if (flow_depth >= 64) return false;
            flow_stack[flow_depth++] = {line_start, line_index, op == OP_WHILE};
        }

        for (uint8_t i = 2; i + 1 < sym_count; i++)
        {
            const ScriptSymbol &os = syms[i];
            if (os.type == SYM_INSTRUCTION)
            {
                if (!is_flow || !IsConditionOp(os.subtype)) return false; // ops only inside conditions
                continue;
            }
            switch (os.type)
            {
                case SYM_INPUT:     if (os.value >= p.header.input_count) return false; break;
                case SYM_OUTPUT:    if (os.value >= p.header.output_count) return false; break;
                case SYM_VARIABLE:  if (os.value >= p.header.variable_count) return false; break;
                case SYM_CONSTANT:  if (os.value >= p.header.constant_count) return false; break;
                case SYM_PREDEFINE: break;
                default: return false;
            }
        }
    }

    if (!has_end || flow_depth != 0) return false;
    p.line_count = (uint16_t)lines;
    return true;
}

// ---------------------------------------------------------------------------
// Runtime value helpers
// ---------------------------------------------------------------------------

static bool PushStack(ScriptInstance &inst, const BlockMeta &meta, const void *data, uint16_t len)
{
    ScriptValueStack &st = inst.stack;
    if (st.used == st.store.map_count)
    {
        if (!st.store.InsertField(st.used, meta)) return false;
    }
    if (!st.store.Set(st.used, data, len, meta.FlagsAndType)) return false;
    st.used++;
    return true;
}

static FieldResult PopStack(ScriptInstance &inst)
{
    if (inst.stack.used == 0) return FieldResult();
    inst.stack.used--;
    return inst.stack.store.Get(inst.stack.used);
}

static bool AsNumber(const FieldResult &f, Number &out)
{
    uint16_t t = BlockMetaType(f.Descriptor.FlagsAndType);
    if (t == (uint16_t)DataType::Number && f.Descriptor.Size >= 4) { out = *(const Number *)f.Data; return true; }
    if (t == (uint16_t)DataType::Index && f.Descriptor.Size >= 4) { out = Number(*(const int32_t *)f.Data); return true; }
    if (t == (uint16_t)DataType::Uint32 && f.Descriptor.Size >= 4) { out = Number((int32_t)(*(const uint32_t *)f.Data)); return true; }
    if (t == (uint16_t)DataType::Bool && f.Descriptor.Size >= 1) { out = Number(((const uint8_t *)f.Data)[0] ? 1 : 0); return true; }
    return false;
}

static bool StackTruth(const FieldResult &f)
{
    if (!f.Data) return false;
    uint16_t t = BlockMetaType(f.Descriptor.FlagsAndType);
    if (t == (uint16_t)DataType::Bool) return ((const uint8_t *)f.Data)[0] != 0;
    if (t == (uint16_t)DataType::Number && f.Descriptor.Size >= 4) return ((const Number *)f.Data)->Value != 0;
    if (t == (uint16_t)DataType::Index && f.Descriptor.Size >= 4) return *(const int32_t *)f.Data != 0;
    return f.Descriptor.Size > 0;
}

// Resolves an operand symbol (Input/Output/Variable/Constant/Predefine) to a value.
// `tmp` is scratch for predefine values (>= 4 bytes).
static bool GetSymbolValue(ScriptInstance &inst, const ScriptSymbol &s,
                           BlockMeta &out_meta, const uint8_t *&out_data, uint16_t &out_len,
                           uint8_t *tmp)
{
    const ScriptProgram &p = inst.program;
    DynamicBlockDescriptor &rt = inst.runtime;
    switch (s.type)
    {
        case SYM_INPUT:
            if (s.value >= p.header.input_count) return false;
            {
                FieldResult f = rt.Get(s.value);
                if (!f.Data) return false;
                out_meta = f.Descriptor; out_data = (const uint8_t *)f.Data; out_len = f.Descriptor.Size;
            }
            return true;
        case SYM_OUTPUT:
            if (s.value >= p.header.output_count) return false;
            {
                FieldResult f = rt.Get((uint16_t)(p.header.input_count + s.value));
                if (!f.Data) return false;
                out_meta = f.Descriptor; out_data = (const uint8_t *)f.Data; out_len = f.Descriptor.Size;
            }
            return true;
        case SYM_VARIABLE:
            if (s.value >= p.header.variable_count) return false;
            {
                FieldResult f = rt.Get((uint16_t)(p.header.input_count + p.header.output_count + s.value));
                if (!f.Data) return false;
                out_meta = f.Descriptor; out_data = (const uint8_t *)f.Data; out_len = f.Descriptor.Size;
            }
            return true;
        case SYM_CONSTANT:
            if (s.value >= p.header.constant_count) return false;
            {
                memcpy(&out_meta, p.constant_meta + s.value * 4, 4);
                out_len = out_meta.Size;
                uint32_t off = 0;
                for (uint16_t i = 0; i < s.value; i++)
                {
                    BlockMeta m;
                    memcpy(&m, p.constant_meta + i * 4, 4);
                    off += AlignTo4(m.Size);
                }
                if (off + out_len > p.header.constant_values_len) return false;
                out_data = p.constant_values + off;
            }
            return true;
        case SYM_PREDEFINE:
            out_meta.Key = 0;
            switch (s.subtype)
            {
                case PRE_BOOL:
                    out_meta.FlagsAndType = (uint16_t)DataType::Bool;
                    out_meta.Size = 1;
                    tmp[0] = (s.value != 0) ? 1 : 0;
                    out_data = tmp; out_len = 1;
                    return true;
                case PRE_CHAR:
                    out_meta.FlagsAndType = (uint16_t)DataType::String;
                    out_meta.Size = 1;
                    tmp[0] = (uint8_t)(s.value & 0xFF);
                    out_data = tmp; out_len = 1;
                    return true;
                case PRE_INDEX:
                case PRE_STATE:
                case PRE_TYPE:
                case PRE_MATHOP:
                    out_meta.FlagsAndType = (uint16_t)DataType::Index;
                    out_meta.Size = 4;
                    tmp[0] = (uint8_t)(s.value & 0xFF);
                    tmp[1] = (uint8_t)(s.value >> 8);
                    tmp[2] = 0; tmp[3] = 0;
                    out_data = tmp; out_len = 4;
                    return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

// Stores a value into the line's output target (Variable or Output field).
static bool StoreResult(ScriptInstance &inst, uint8_t out_kind, uint16_t out_idx,
                        const BlockMeta &meta, const void *data, uint16_t len)
{
    uint16_t field;
    if (out_kind == SYM_OUTPUT)
        field = (uint16_t)(inst.program.header.input_count + out_idx);
    else
        field = (uint16_t)(inst.program.header.input_count + inst.program.header.output_count + out_idx);
    return inst.runtime.Set(field, data, len, meta.FlagsAndType);
}

// Seeds the runtime block from the program: input fields carry the file's key metas and
// default values; outputs and variables start as empty None fields (stable indexes).
static bool RuntimeSeed(ScriptInstance &inst)
{
    const ScriptFileHeader &h = inst.program.header;
    DynamicBlockDescriptor &rt = inst.runtime;
    // Per-input meta stride: 5 bytes (meta + style byte) in the new format, 4 in legacy.
    uint32_t step = inst.program.input_meta_new ? 5 : 4;
    uint32_t cursor = 4; // skip the dict BlockMeta
    uint32_t vcursor = 0;
    for (uint8_t i = 0; i < h.input_count; i++)
    {
        if (cursor + 4 > h.input_meta_len) return false;
        BlockMeta meta;
        memcpy(&meta, inst.program.input_meta + cursor, 4);
        cursor += step;
        if (vcursor + AlignTo4(meta.Size) > h.input_value_len) return false;
        if (!rt.InsertField(i, meta)) return false;
        if (meta.Size)
        {
            if (!rt.Set(i, inst.program.input_values + vcursor, meta.Size, meta.FlagsAndType))
                return false;
        }
        vcursor += AlignTo4(meta.Size);
    }
    uint16_t total = (uint16_t)h.input_count + h.output_count + h.variable_count;
    for (uint16_t i = h.input_count; i < total; i++)
    {
        BlockMeta none = {(uint16_t)DataType::None, 0, 0};
        if (!rt.InsertField(i, none)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Memory service access from scripts (Docs/Services/Script.md "System & Dynamic
// block reader and writer"). The address is a 4-byte value [block][field][key][service]
// where service is the target ServiceType (SystemMemory/DynamicMemory/KeyedMemory).
// ---------------------------------------------------------------------------

static bool ResolveMemRead(uint8_t block, uint8_t field, uint8_t key, uint8_t svc,
                           BlockMeta &meta, const uint8_t *&data, uint16_t &len)
{
    switch ((ServiceType)svc)
    {
        case ServiceType::SystemMemory:
            if (block >= static_block_num || field == INVALID_INDEX) return false;
            {
                FieldResult f = static_block_registry[block].Get(field);
                if (!f.Data) return false;
                meta = f.Descriptor; data = (const uint8_t *)f.Data; len = f.Descriptor.Size;
            }
            return true;
        case ServiceType::DynamicMemory:
            {
                DynamicBlockDescriptor *b = dynamic_block_registry.GetBlock(block);
                if (!b || field == INVALID_INDEX) return false;
                FieldResult f = b->Get(field);
                if (!f.Data) return false;
                meta = f.Descriptor; data = (const uint8_t *)f.Data; len = f.Descriptor.Size;
            }
            return true;
        case ServiceType::KeyedMemory:
            {
                KeyedBlockDescriptor *b = (KeyedBlockDescriptor *)keyed_block_registry.GetBlock(block);
                if (!b || field == INVALID_INDEX) return false;
                if (key == INVALID_INDEX)
                {
                    FieldResult f = b->Get(field);
                    if (!f.Data) return false;
                    meta = f.Descriptor; data = (const uint8_t *)f.Data; len = f.Descriptor.Size;
                    return true;
                }
                KeyResult r = b->GetKey(field, key);
                if (!r.data_ptr) return false;
                meta = r.meta; data = (const uint8_t *)r.data_ptr; len = r.data_len;
            }
            return true;
        default:
            return false;
    }
}

static bool ResolveMemWrite(uint8_t block, uint8_t field, uint8_t key, uint8_t svc,
                            const FieldResult &val)
{
    switch ((ServiceType)svc)
    {
        case ServiceType::SystemMemory:
            if (block >= static_block_num || field == INVALID_INDEX) return false;
            return static_block_registry[block].Set(field, val.Data, val.Descriptor.Size,
                                                    val.Descriptor.FlagsAndType);
        case ServiceType::DynamicMemory:
            {
                DynamicBlockDescriptor *b = dynamic_block_registry.GetBlock(block);
                if (!b || field == INVALID_INDEX) return false;
                if (field >= b->map_count)
                {
                    // Scripts write contiguous fields; create a missing one like the
                    // Dynamic Memory service's Write (append at the end only).
                    if (field != b->map_count) return false;
                    BlockMeta meta = val.Descriptor;
                    meta.Size = val.Descriptor.Size;
                    if (!b->InsertField(field, meta)) return false;
                }
                if (!b->Set(field, val.Data, val.Descriptor.Size, val.Descriptor.FlagsAndType))
                    return false;
                // Script-updated values are RAM-only: saved only on explicit request
                // (Docs/Data Formats.md).
                b->map[field].FlagsAndType |= FieldFlags::ScriptUpdated;
            }
            return true;
        case ServiceType::KeyedMemory:
            {
                KeyedBlockDescriptor *b = (KeyedBlockDescriptor *)keyed_block_registry.GetBlock(block);
                if (!b || field == INVALID_INDEX || key == INVALID_INDEX) return false;
                if (!b->SetKey(field, key, val.Data, val.Descriptor.Size, val.Descriptor.FlagsAndType))
                    return false;
                KeyResult r = b->GetKey(field, key);
                if (r.data_ptr)
                {
                    BlockMeta *m = (BlockMeta *)((uint8_t *)r.data_ptr - sizeof(BlockMeta));
                    m->FlagsAndType |= FieldFlags::ScriptUpdated;
                }
            }
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Instruction execution
// ---------------------------------------------------------------------------

enum ExecResult : uint8_t
{
    EXEC_CONTINUE = 0, // keep running (counter may have been set by a jump)
    EXEC_STOP = 1      // stop this tick: error, finish, wait, or state change
};

static bool ScriptSetError(ScriptInstance &inst, uint32_t line, const char *why)
{
    inst.state = SCRIPT_ERROR;
    DeviceLog("SCRIPT", "script %u error at line %lu: %s", (unsigned)inst.script_id,
              (unsigned long)line, why);
    ReportLog(MakeLog(false, (uint16_t)ServiceType::Script, inst.script_id, 1));
    return false;
}

// Sets the error state and returns EXEC_STOP (for ExecResult-returning functions).
static ExecResult ScriptFail(ScriptInstance &inst, uint32_t line, const char *why)
{
    ScriptSetError(inst, line, why);
    return EXEC_STOP;
}

// Executes a math/logic instruction on the operand stack (used inside conditions and
// by the primary math ops through ExecuteOp's store path).
static bool ExecMathOp(ScriptInstance &inst, uint8_t op, bool store, uint8_t out_kind, uint16_t out_idx)
{
    uint8_t tmp8[8];
    if (op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV)
    {
        FieldResult b = PopStack(inst), a = PopStack(inst);
        if (!a.Data || !b.Data) return false;
        Number na, nb;
        if (!AsNumber(a, na) || !AsNumber(b, nb)) return false;
        Number r;
        switch (op)
        {
            case OP_ADD: r = na + nb; break;
            case OP_SUB: r = na - nb; break;
            case OP_MUL: r = na * nb; break;
            default:     r = na / nb; break;
        }
        if (store)
        {
            BlockMeta m = {(uint16_t)DataType::Number, 0, 4};
            return StoreResult(inst, out_kind, out_idx, m, &r, 4);
        }
        return PushStack(inst, {(uint16_t)DataType::Number, 0, 4}, &r, 4);
    }
    if (op == OP_NEG)
    {
        FieldResult a = PopStack(inst);
        Number na;
        if (!a.Data || !AsNumber(a, na)) return false;
        Number r = -na;
        if (store) { BlockMeta m = {(uint16_t)DataType::Number, 0, 4}; return StoreResult(inst, out_kind, out_idx, m, &r, 4); }
        return PushStack(inst, {(uint16_t)DataType::Number, 0, 4}, &r, 4);
    }
    if (op == OP_AND || op == OP_OR)
    {
        FieldResult b = PopStack(inst), a = PopStack(inst);
        if (!a.Data || !b.Data) return false;
        bool r = (op == OP_AND) ? (StackTruth(a) && StackTruth(b)) : (StackTruth(a) || StackTruth(b));
        tmp8[0] = r ? 1 : 0;
        if (store) { BlockMeta m = {(uint16_t)DataType::Bool, 0, 1}; return StoreResult(inst, out_kind, out_idx, m, tmp8, 1); }
        return PushStack(inst, {(uint16_t)DataType::Bool, 0, 1}, tmp8, 1);
    }
    if (op == OP_NOT)
    {
        FieldResult a = PopStack(inst);
        if (!a.Data) return false;
        tmp8[0] = StackTruth(a) ? 0 : 1;
        if (store) { BlockMeta m = {(uint16_t)DataType::Bool, 0, 1}; return StoreResult(inst, out_kind, out_idx, m, tmp8, 1); }
        return PushStack(inst, {(uint16_t)DataType::Bool, 0, 1}, tmp8, 1);
    }
    if (op >= OP_CMP_EQ && op <= OP_CMP_GE)
    {
        FieldResult b = PopStack(inst), a = PopStack(inst);
        if (!a.Data || !b.Data) return false;
        Number na, nb;
        if (!AsNumber(a, na) || !AsNumber(b, nb)) return false;
        bool r;
        switch (op)
        {
            case OP_CMP_EQ: r = (na == nb); break;
            case OP_CMP_NE: r = (na != nb); break;
            case OP_CMP_LT: r = (na < nb); break;
            case OP_CMP_LE: r = (na <= nb); break;
            case OP_CMP_GT: r = (na > nb); break;
            default:        r = (na >= nb); break;
        }
        tmp8[0] = r ? 1 : 0;
        if (store) { BlockMeta m = {(uint16_t)DataType::Bool, 0, 1}; return StoreResult(inst, out_kind, out_idx, m, tmp8, 1); }
        return PushStack(inst, {(uint16_t)DataType::Bool, 0, 1}, tmp8, 1);
    }
    return false;
}

// Evaluates an If/While condition: an embedded math/logic expression leaving one Bool.
static bool EvalCondition(ScriptInstance &inst, uint32_t offset, bool &result)
{
    const ScriptProgram &p = inst.program;
    uint8_t tmp[16];
    inst.stack.used = 0;
    for (;;)
    {
        ScriptSymbol s;
        if (!ScriptSymbolRead(p.instructions, p.header.instruction_len, offset, &s)) return false;
        if (s.type == SYM_ENDLINE) break;
        if (s.type == SYM_INSTRUCTION)
        {
            if (!ExecMathOp(inst, s.subtype, false, 0, 0)) return false;
        }
        else
        {
            BlockMeta m;
            const uint8_t *d;
            uint16_t l;
            if (!GetSymbolValue(inst, s, m, d, l, tmp)) return false;
            if (!PushStack(inst, m, d, l)) return false;
        }
        offset += 4;
    }
    FieldResult top = PopStack(inst);
    if (!top.Data) return false;
    result = StackTruth(top);
    return true;
}

static bool ExecComposeVec(ScriptInstance &inst, uint16_t n, uint8_t out_kind, uint16_t out_idx)
{
    if (n == 0 || n > 64) return false;
    Number vals[64];
    for (uint16_t i = n; i-- > 0;)
    {
        FieldResult f = PopStack(inst);
        if (!f.Data || !AsNumber(f, vals[i])) return false;
    }
    BlockMeta m = {(uint16_t)DataType::Vector, 0, (uint8_t)(n * 4)};
    return StoreResult(inst, out_kind, out_idx, m, vals, n * 4);
}

static bool ExecComposeColour(ScriptInstance &inst, uint8_t out_kind, uint16_t out_idx)
{
    Number vals[4];
    for (uint16_t i = 4; i-- > 0;)
    {
        FieldResult f = PopStack(inst);
        if (!f.Data || !AsNumber(f, vals[i])) return false;
    }
    uint8_t rgba[4];
    for (int i = 0; i < 4; i++)
        rgba[i] = LimitByte(vals[i].RoundToInt());
    BlockMeta m = {(uint16_t)DataType::Colour, 0, 4};
    return StoreResult(inst, out_kind, out_idx, m, rgba, 4);
}

static bool ExecExtract(ScriptInstance &inst, uint8_t out_kind, uint16_t out_idx)
{
    FieldResult idx_f = PopStack(inst), cont = PopStack(inst);
    Number n;
    if (!cont.Data || !idx_f.Data || !AsNumber(idx_f, n)) return false;
    int32_t idx = n.ToInt();
    uint16_t t = BlockMetaType(cont.Descriptor.FlagsAndType);
    const uint8_t *data = (const uint8_t *)cont.Data;
    uint16_t len = cont.Descriptor.Size;
    Number r;
    if (t == (uint16_t)DataType::Vector)
    {
        if (idx < 0 || (uint32_t)idx >= len / 4) return false;
        r = ((const Number *)data)[idx];
    }
    else if (t == (uint16_t)DataType::Colour)
    {
        if (idx < 0 || (uint32_t)idx >= len) return false;
        r = Number(data[idx]);
    }
    else if (t == (uint16_t)DataType::Matrix)
    {
        if (idx < 0 || (uint32_t)idx >= len / 4) return false;
        r = ((const Number *)data)[idx];
    }
    else
    {
        return false;
    }
    BlockMeta m = {(uint16_t)DataType::Number, 0, 4};
    return StoreResult(inst, out_kind, out_idx, m, &r, 4);
}

static bool ExecMemRead(ScriptInstance &inst, uint8_t out_kind, uint16_t out_idx)
{
    FieldResult addr_f = PopStack(inst);
    if (!addr_f.Data || addr_f.Descriptor.Size < 4) return false;
    const uint8_t *a = (const uint8_t *)addr_f.Data;
    BlockMeta m;
    const uint8_t *val;
    uint16_t vlen;
    if (!ResolveMemRead(a[0], a[1], a[2], a[3], m, val, vlen)) return false;
    return StoreResult(inst, out_kind, out_idx, m, val, vlen);
}

static bool ExecMemWrite(ScriptInstance &inst, uint8_t out_kind, uint16_t out_idx)
{
    // Operands are (value, address) in line order; the stack top is the address.
    FieldResult addr_f = PopStack(inst), val_f = PopStack(inst);
    if (!val_f.Data || !addr_f.Data || addr_f.Descriptor.Size < 4) return false;
    const uint8_t *a = (const uint8_t *)addr_f.Data;
    bool ok = ResolveMemWrite(a[0], a[1], a[2], a[3], val_f);
    uint8_t succ = ok ? 1 : 0;
    BlockMeta m = {(uint16_t)DataType::Bool, 0, 1};
    return StoreResult(inst, out_kind, out_idx, m, &succ, 1);
}

// Executes the primary instruction of a line. Operands are on the stack. Returns EXEC_STOP
// when the tick must stop (wait/state change), EXEC_CONTINUE otherwise.
static ExecResult ExecuteOp(ScriptInstance &inst, uint8_t op, uint16_t op_value,
                            uint8_t out_kind, uint16_t out_idx)
{
    switch (op)
    {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_NEG:
        case OP_AND: case OP_OR: case OP_NOT:
        case OP_CMP_EQ: case OP_CMP_NE: case OP_CMP_LT: case OP_CMP_LE: case OP_CMP_GT: case OP_CMP_GE:
            if (!ExecMathOp(inst, op, true, out_kind, out_idx)) return ScriptFail(inst, inst.counter, "math op");
            return EXEC_CONTINUE;
        case OP_COMPOSE_VEC:
            if (!ExecComposeVec(inst, op_value, out_kind, out_idx)) return ScriptFail(inst, inst.counter, "compose vec");
            return EXEC_CONTINUE;
        case OP_COMPOSE_COLOUR:
            if (!ExecComposeColour(inst, out_kind, out_idx)) return ScriptFail(inst, inst.counter, "compose colour");
            return EXEC_CONTINUE;
        case OP_EXTRACT:
            if (!ExecExtract(inst, out_kind, out_idx)) return ScriptFail(inst, inst.counter, "extract");
            return EXEC_CONTINUE;
        case OP_MEM_READ:
            if (!ExecMemRead(inst, out_kind, out_idx)) return ScriptFail(inst, inst.counter, "mem read");
            return EXEC_CONTINUE;
        case OP_MEM_WRITE:
            if (!ExecMemWrite(inst, out_kind, out_idx)) return ScriptFail(inst, inst.counter, "mem write");
            return EXEC_CONTINUE;
        case OP_DELAY:
            {
                FieldResult f = PopStack(inst);
                Number ms;
                if (!f.Data || !AsNumber(f, ms)) return ScriptFail(inst, inst.counter, "delay");
                inst.wake_time = Now() + (uint32_t)ms.ToInt();
                inst.input_wake = false;
                inst.state = SCRIPT_WAITING;
            }
            return EXEC_STOP;
        case OP_GET_TIME:
            {
                uint32_t t = DeviceStatus.UptimeMs;
                BlockMeta m = {(uint16_t)DataType::Uint32, 0, 4};
                if (!StoreResult(inst, out_kind, out_idx, m, &t, 4)) return ScriptFail(inst, inst.counter, "store");
            }
            return EXEC_CONTINUE;
        case OP_PAUSE:
            inst.state = SCRIPT_PAUSED;
            return EXEC_STOP;
        case OP_RESUME:
            inst.state = SCRIPT_RUNNING;
            inst.tick_start = 0;
            return EXEC_STOP;
        case OP_TERMINATE:
            inst.state = SCRIPT_STOPPED;
            inst.counter = 0;
            return EXEC_STOP;
        case OP_RESTART:
            inst.state = SCRIPT_RUNNING;
            inst.counter = 0;
            inst.tick_start = 0;
            return EXEC_STOP;
        case OP_INFO_REPORT:
            DeviceLog("SCRIPT", "script %u report at line %lu", (unsigned)inst.script_id,
                      (unsigned long)inst.counter);
            ReportLog(MakeLog(false, (uint16_t)ServiceType::Script, inst.script_id, 2));
            return EXEC_CONTINUE;
        case OP_ERROR_HALT:
            return ScriptFail(inst, inst.counter, "error halt");
        default:
            return ScriptFail(inst, inst.counter, "bad opcode");
    }
}

// Executes one line of the script at `line`. The instance counter is used as the result
// location for jumps; the caller decides whether to continue based on state + loop-back.
static ExecResult ScriptExecLine(ScriptInstance &inst, uint32_t line)
{
    ScriptProgram &p = inst.program;
    uint32_t offset = ScriptLineOffset(p, line);
    uint32_t len = p.header.instruction_len;
    uint8_t tmp[16];

    ScriptSymbol first;
    if (!ScriptSymbolRead(p.instructions, len, offset, &first))
        return ScriptFail(inst, line, "truncated line");

    // Degenerate flow terminator: [EndIf|EndWhile|End] EndLine
    if (first.type == SYM_INSTRUCTION &&
        (first.subtype == OP_END_IF || first.subtype == OP_END_WHILE || first.subtype == OP_END))
    {
        if (first.subtype == OP_END)
        {
            if (inst.macro_depth > 0)
            {
                // Return from a macro: restore the caller's program/runtime/counter.
                ScriptMacroFrame &fr = inst.macro_stack[inst.macro_depth - 1];
                inst.program.Release();
                inst.runtime.Release();
                inst.program = fr.program;
                inst.runtime = fr.runtime;
                fr.program = ScriptProgram{};
                fr.runtime = DynamicBlockDescriptor{};
                inst.counter = fr.counter;
                inst.macro_depth--;
                return EXEC_CONTINUE;
            }
            inst.state = SCRIPT_FINISHED;
            return EXEC_STOP;
        }
        if (first.subtype == OP_END_WHILE)
        {
            inst.counter = first.value; // loop back to the matching While line
            return EXEC_CONTINUE;
        }
        return EXEC_CONTINUE; // EndIf: fall through
    }

    // Output symbol
    if (first.type != SYM_OUTPUT && first.type != SYM_VARIABLE)
        return ScriptFail(inst, line, "line must start with an output");
    uint8_t out_kind = first.type;
    uint16_t out_idx = first.value;
    if (out_kind == SYM_OUTPUT && out_idx >= p.header.output_count)
        return ScriptFail(inst, line, "output index out of range");
    if (out_kind == SYM_VARIABLE && out_idx >= p.header.variable_count)
        return ScriptFail(inst, line, "variable index out of range");

    offset += 4;
    ScriptSymbol op_sym;
    if (!ScriptSymbolRead(p.instructions, len, offset, &op_sym))
        return ScriptFail(inst, line, "truncated line");
    if (op_sym.type != SYM_INSTRUCTION || op_sym.subtype >= OP_MAX)
        return ScriptFail(inst, line, "expected instruction");
    uint8_t op = op_sym.subtype;
    offset += 4;

    // If/While: the remaining symbols form an embedded expression -> one Bool.
    if (op == OP_IF || op == OP_WHILE)
    {
        bool cond = false;
        if (!EvalCondition(inst, offset, cond))
            return ScriptFail(inst, line, "condition failed");
        if (!cond)
            inst.counter = op_sym.value; // patched exit line (after the matching End)
        return EXEC_CONTINUE;
    }

    // Normal op: push the operand symbols (instruction symbols are rejected - no chaining).
    inst.stack.used = 0;
    for (;;)
    {
        ScriptSymbol s;
        if (!ScriptSymbolRead(p.instructions, len, offset, &s))
            return ScriptFail(inst, line, "truncated line");
        if (s.type == SYM_ENDLINE) break;
        if (s.type == SYM_INSTRUCTION)
            return ScriptFail(inst, line, "chained instruction in operands");
        BlockMeta m;
        const uint8_t *d;
        uint16_t l;
        if (!GetSymbolValue(inst, s, m, d, l, tmp))
            return ScriptFail(inst, line, "bad operand");
        if (!PushStack(inst, m, d, l))
            return ScriptFail(inst, line, "stack overflow");
        offset += 4;
    }

    return ExecuteOp(inst, op, op_sym.value, out_kind, out_idx);
}