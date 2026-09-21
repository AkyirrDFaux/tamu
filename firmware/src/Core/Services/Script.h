#pragma once

// Script service (Docs/Services/Script.md).
//
// Milestone A: SCR_XX file parsing, the loaded-script registry, Register exposure (block
// type 0x3FE) and management commands 0x0500-0x0507.
// Milestone B: preloaded instructions, line/block tables, the execution engine (the VM)
// and the six-state machine. The opcode/symbol encoding is an internal convention shared
// with the app's script_instructions.dart.

#ifdef USE_SCRIPTS

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "Core/Functions/Memory.h"
#include "Core/Functions/Storage.h"
#include "Core/Types/Enums.h"
#include "Core/Types/Number.h"

// Defined in Core/Services/Register.h (shared with the Subscriptions service); declared
// here so the VM resolves registers through the exact same access path.
bool RegisterGetByBlockInfo(uint32_t bi, BlockMeta &m, uint8_t *vbuf, uint8_t &vsz);
bool RegisterSetByBlockInfo(uint32_t bi, const BlockMeta &m, const uint8_t *val, uint16_t vlen);

// Number of script file slots (SCR_00..SCR_3F); the slot index doubles as the Script File
// ID and as the Register instance number of block type 0x3FE.
#define MAX_SCRIPTS 64
#define SCRIPT_HEADER_SIZE 24
#define SCRIPT_SYMBOL_SIZE 4
#define SCRIPT_MAX_LINES 1024
#define SCRIPT_INSTR_BUDGET 512
#define SCRIPT_MAX_CALL_DEPTH 8

// Symbol types (Docs symbol table).
#define SCRIPT_SYM_INSTRUCTION 0
#define SCRIPT_SYM_INPUT 1
#define SCRIPT_SYM_OUTPUT 2
#define SCRIPT_SYM_VARIABLE 3
#define SCRIPT_SYM_CONSTANT 4
#define SCRIPT_SYM_ENDLINE 5
#define SCRIPT_SYM_PREDEFINE 6

// Instruction categories (the instruction symbol's subtype).
#define SCRIPT_CAT_MATH 0
#define SCRIPT_CAT_LOGIC 1
#define SCRIPT_CAT_FLOW 2
#define SCRIPT_CAT_TIME 3
#define SCRIPT_CAT_SERVICE 4
#define SCRIPT_CAT_COMPOSE 5

// Predefine subtypes.
#define SCRIPT_PRE_STATE 0
#define SCRIPT_PRE_TYPE 1
#define SCRIPT_PRE_INDEX 2
#define SCRIPT_PRE_CHAR 3
#define SCRIPT_PRE_MATHOP 4
#define SCRIPT_PRE_BOOL 5

// Math ops.
#define SCRIPT_OP_MATH_SET 0
#define SCRIPT_OP_MATH_ADD 1
#define SCRIPT_OP_MATH_SUB 2
#define SCRIPT_OP_MATH_MUL 3
#define SCRIPT_OP_MATH_DIV 4
#define SCRIPT_OP_MATH_MOD 5
#define SCRIPT_OP_MATH_MIN 6
#define SCRIPT_OP_MATH_MAX 7
#define SCRIPT_OP_MATH_NEG 8
#define SCRIPT_OP_MATH_ABS 9

// Logic ops.
#define SCRIPT_OP_LOGIC_AND 0
#define SCRIPT_OP_LOGIC_OR 1
#define SCRIPT_OP_LOGIC_XOR 2
#define SCRIPT_OP_LOGIC_NOT 3
#define SCRIPT_OP_LOGIC_SHL 4
#define SCRIPT_OP_LOGIC_SHR 5
#define SCRIPT_OP_LOGIC_CMP_EQ 6
#define SCRIPT_OP_LOGIC_CMP_NE 7
#define SCRIPT_OP_LOGIC_CMP_LT 8
#define SCRIPT_OP_LOGIC_CMP_LE 9
#define SCRIPT_OP_LOGIC_CMP_GT 10
#define SCRIPT_OP_LOGIC_CMP_GE 11
#define SCRIPT_OP_LOGIC_SELECT 12

// Flow ops.
#define SCRIPT_OP_FLOW_IF 0
#define SCRIPT_OP_FLOW_WHILE 1
#define SCRIPT_OP_FLOW_END 2
#define SCRIPT_OP_FLOW_JUMP 3
#define SCRIPT_OP_FLOW_CALL 4
#define SCRIPT_OP_FLOW_RETURN 5
#define SCRIPT_OP_FLOW_HALT 6

// Time ops.
#define SCRIPT_OP_TIME_DELAY 0
#define SCRIPT_OP_TIME_WAIT 1
#define SCRIPT_OP_TIME_GET 2

// Service ops.
#define SCRIPT_OP_SERVICE_LOG 0
#define SCRIPT_OP_SERVICE_REG_READ 1
#define SCRIPT_OP_SERVICE_REG_WRITE 2
#define SCRIPT_OP_SERVICE_STATE 3
#define SCRIPT_OP_SERVICE_NOP 4
#define SCRIPT_OP_SERVICE_REG_READ_FOREIGN 5
#define SCRIPT_OP_SERVICE_REG_WRITE_FOREIGN 6

// Compose ops.
#define SCRIPT_OP_COMPOSE 0
#define SCRIPT_OP_EXTRACT 1

// Error codes (surfaced through the header's Error entry).
#define SCRIPT_ERR_NONE 0
#define SCRIPT_ERR_UNKNOWN_OP 1
#define SCRIPT_ERR_TYPE 2
#define SCRIPT_ERR_OPERAND 3
#define SCRIPT_ERR_BOUNDS 4
#define SCRIPT_ERR_STACK 5
#define SCRIPT_ERR_REGISTER 6
#define SCRIPT_ERR_TIMEOUT 7
#define SCRIPT_ERR_NOT_IMPL 8

// Per-instance script transaction IDs for foreign register access.
#define SCRIPT_TRID_BASE 0xF000
#define SCRIPT_TRID_MAX  0xF9FF

// States (Docs/Services/Script.md "State").
enum class ScriptState : uint8_t {
    Stopped  = 0,
    Running  = 1,
    Paused   = 2,
    Waiting  = 3,
    Finished = 4,
    Error    = 5
};

// Properties bits ("information about script type (load on boot etc..)").
#define SCRIPT_PROP_LOAD_ON_BOOT (1u << 0)
#define SCRIPT_PROP_RUN_ON_LOAD  (1u << 1)

// Register field categories for block type 0x3FE (Scripts). Only the I/O categories are
// exposed through the Register (docs: "IO is in register"); the Header, Variables and
// Constants are script metadata reached through the Script management commands.
#define SCRIPT_FIELD_HEADER   0
#define SCRIPT_FIELD_INPUT    1
#define SCRIPT_FIELD_OUTPUT   2
#define SCRIPT_FIELD_VARIABLE 3 // internal only (CID 5/7), not in the Register
#define SCRIPT_FIELD_CONSTANT 4 // internal only, not in the Register
#define SCRIPT_FIELD_COUNT    3 // register categories: Header (reserved) + Input + Output

// Header keys (field 0).
#define SCRIPT_KEY_STATE       0
#define SCRIPT_KEY_IC          1
#define SCRIPT_KEY_PROPERTIES  2
#define SCRIPT_KEY_IN_COUNT    3
#define SCRIPT_KEY_OUT_COUNT   4
#define SCRIPT_KEY_VAR_COUNT   5
#define SCRIPT_KEY_CONST_COUNT 6
#define SCRIPT_KEY_FILE_ID     7
#define SCRIPT_KEY_ERROR       8
#define SCRIPT_HEADER_KEYS     9

static inline uint16_t ScriptAlign4(uint16_t size) { return (uint16_t)((size + 3u) & ~3u); }

// SCR_XX (8-char space-padded name, the slot index in hex).
static inline void ScriptFileName(uint8_t id, char out[8]) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = 'S'; out[1] = 'C'; out[2] = 'R'; out[3] = '_';
    out[4] = hex[(id >> 4) & 0x0F];
    out[5] = hex[id & 0x0F];
    out[6] = ' '; out[7] = ' ';
}

static inline uint32_t ScriptRdU32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

// Precompiled line descriptor: the instruction symbol sits at start+destCount, its
// operands follow, and the line ends with an Endline symbol.
struct ScriptLineInfo {
    uint16_t start;
    uint8_t destCount;
    uint8_t opCount;
};

// One loaded script. Value spaces are parallel to the register tables:
//   ioSpace    : inputs then outputs, 4-byte strided (volatile)
//   varSpace   : leading u32 instruction counter + variables
//   constSpace : constants (read-only)
struct LoadedScript {
    bool active = false;
    uint8_t slot = 0;
    uint8_t state = (uint8_t)ScriptState::Stopped;
    uint32_t properties = 0;

    uint8_t inCount = 0, outCount = 0, varCount = 0, constCount = 0;
    BlockMeta *inMeta = nullptr, *outMeta = nullptr, *varMeta = nullptr, *constMeta = nullptr;

    uint8_t *ioSpace = nullptr;
    uint8_t *varSpace = nullptr;
    uint8_t *constSpace = nullptr;
    uint16_t inTotal = 0, outTotal = 0, varTotal = 0, constTotal = 0;

    uint16_t uiLen = 0;
    uint32_t instrLen = 0;
    char name[BLOCK_NAME_LEN] = {0};

    // Program (preloaded instructions + compiled line/block tables).
    uint8_t *instr = nullptr;
    uint16_t instrSymbols = 0;
    ScriptLineInfo *lines = nullptr;
    uint16_t lineCount = 0;
    uint16_t *blockMatch = nullptr; // per line: matching If/While <-> EndBlock (0xFFFF none)
    uint8_t *blockKind = nullptr;   // 0 none, 1 If, 2 While, 3 EndBlock
    uint32_t *lineStamp = nullptr;  // loop-detection stamp per line

    // Runtime state.
    uint16_t ic = 0; // line index
    bool waitingOnTime = false;
    uint32_t waitUntil = 0;
    uint16_t callStack[SCRIPT_MAX_CALL_DEPTH] = {0};
    uint8_t callDepth = 0;
    uint8_t errorCode = SCRIPT_ERR_NONE;

    // Foreign register access (per-instance TRID + pending confirmation).
    uint16_t trid = 0;
    bool pendingForeign = false;
    bool pendingRead = false;
    uint16_t pendingTrid = 0;
    uint32_t pendingDeadline = 0;
    uint8_t pendingDest[4] = {0};

    void Release() {
        free(inMeta); free(outMeta); free(varMeta); free(constMeta);
        free(ioSpace); free(varSpace); free(constSpace);
        free(instr); free(lines); free(blockMatch); free(blockKind); free(lineStamp);
        inMeta = outMeta = varMeta = constMeta = nullptr;
        ioSpace = varSpace = constSpace = nullptr;
        instr = nullptr; lines = nullptr; blockMatch = nullptr; blockKind = nullptr; lineStamp = nullptr;
        active = false;
        state = (uint8_t)ScriptState::Stopped;
        properties = 0;
        inCount = outCount = varCount = constCount = 0;
        inTotal = outTotal = varTotal = constTotal = 0;
        uiLen = 0; instrLen = 0; instrSymbols = 0; lineCount = 0;
        ic = 0; waitingOnTime = false; waitUntil = 0;
        callDepth = 0; errorCode = SCRIPT_ERR_NONE;
        trid = 0; pendingForeign = false; pendingRead = false; pendingTrid = 0; pendingDeadline = 0;
        memset(pendingDest, 0, sizeof(pendingDest));
        name[0] = '\0';
    }

    uint16_t InputOffset(uint8_t i) const {
        uint16_t off = 0;
        for (uint8_t k = 0; k < i && k < inCount; k++) off += ScriptAlign4(inMeta[k].Size);
        return off;
    }
    uint16_t OutputOffset(uint8_t i) const {
        uint16_t off = inTotal;
        for (uint8_t k = 0; k < i && k < outCount; k++) off += ScriptAlign4(outMeta[k].Size);
        return off;
    }
    uint16_t VarOffset(uint8_t i) const {
        uint16_t off = 4;
        for (uint8_t k = 0; k < i && k < varCount; k++) off += ScriptAlign4(varMeta[k].Size);
        return off;
    }
    uint16_t ConstOffset(uint8_t i) const {
        uint16_t off = 0;
        for (uint8_t k = 0; k < i && k < constCount; k++) off += ScriptAlign4(constMeta[k].Size);
        return off;
    }
};

static LoadedScript scriptRegistry[MAX_SCRIPTS];

static LoadedScript *ScriptActive(uint8_t slot) {
    if (slot >= MAX_SCRIPTS) return nullptr;
    return scriptRegistry[slot].active ? &scriptRegistry[slot] : nullptr;
}

static uint16_t ScriptSumStrides(const BlockMeta *metas, uint8_t count) {
    uint16_t total = 0;
    for (uint8_t i = 0; i < count; i++) total += ScriptAlign4(metas[i].Size);
    return total;
}

static BlockMeta *ScriptAllocMetas(uint8_t count) {
    if (count == 0) return nullptr;
    return (BlockMeta *)calloc(count, sizeof(BlockMeta));
}

// ===== Program compilation (instructions -> lines + If/While block table) =====

static bool ScriptBuildProgram(LoadedScript *s, const uint8_t *instr, uint32_t instrLen) {
    s->instrSymbols = (uint16_t)(instrLen / 4);
    if (s->instrSymbols) {
        s->instr = (uint8_t *)malloc((size_t)s->instrSymbols * 4);
        if (!s->instr) return false;
        memcpy(s->instr, instr, (size_t)s->instrSymbols * 4);
    }

    // First pass: count lines.
    uint16_t count = 0;
    uint16_t i = 0;
    while (i < s->instrSymbols) {
        while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_INSTRUCTION &&
               s->instr[i * 4] != SCRIPT_SYM_ENDLINE) {
            i++;
        }
        if (i >= s->instrSymbols || s->instr[i * 4] != SCRIPT_SYM_INSTRUCTION) {
            while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
            if (i < s->instrSymbols) i++;
            continue;
        }
        i++; // instruction
        while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
        if (i < s->instrSymbols) i++; // endline
        count++;
        if (count > SCRIPT_MAX_LINES) return false;
    }

    s->lineCount = count;
    if (count == 0) return true;
    s->lines = (ScriptLineInfo *)calloc(count, sizeof(ScriptLineInfo));
    s->blockMatch = (uint16_t *)malloc((size_t)count * sizeof(uint16_t));
    s->blockKind = (uint8_t *)calloc(count, 1);
    s->lineStamp = (uint32_t *)calloc(count, sizeof(uint32_t));
    if (!s->lines || !s->blockMatch || !s->blockKind || !s->lineStamp) return false;

    // Second pass: fill line descriptors and match If/While blocks. Mirrors the first
    // pass exactly (including orphan-symbol skipping) so indexes stay aligned.
    uint16_t *stack = (uint16_t *)malloc((size_t)count * sizeof(uint16_t));
    if (!stack) return false;
    uint16_t sp = 0;
    uint16_t line = 0;
    i = 0;
    while (line < count) {
        uint16_t start = i;
        while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_INSTRUCTION &&
               s->instr[i * 4] != SCRIPT_SYM_ENDLINE) {
            i++;
        }
        if (i >= s->instrSymbols || s->instr[i * 4] != SCRIPT_SYM_INSTRUCTION) {
            while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
            if (i < s->instrSymbols) i++;
            continue;
        }
        uint8_t destCount = (uint8_t)(i - start);
        uint8_t cat = s->instr[i * 4 + 1];
        uint8_t op = s->instr[i * 4 + 2];
        i++; // instruction
        uint16_t opStart = i;
        while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
        uint8_t opCount = (uint8_t)(i - opStart);
        if (i < s->instrSymbols) i++; // endline

        s->lines[line].start = start;
        s->lines[line].destCount = destCount;
        s->lines[line].opCount = opCount;
        s->blockMatch[line] = 0xFFFF;

        if (cat == SCRIPT_CAT_FLOW && (op == SCRIPT_OP_FLOW_IF || op == SCRIPT_OP_FLOW_WHILE)) {
            s->blockKind[line] = (op == SCRIPT_OP_FLOW_IF) ? 1 : 2;
            if (sp < count) stack[sp++] = line;
        } else if (cat == SCRIPT_CAT_FLOW && op == SCRIPT_OP_FLOW_END) {
            s->blockKind[line] = 3;
            if (sp > 0) {
                uint16_t open = stack[--sp];
                s->blockMatch[open] = line;
                s->blockMatch[line] = open;
            }
        }
        line++;
    }
    free(stack);
    return true;
}

// Parses SCR_<fileId> into the registry slot with the same index. Returns false (and leaves
// the slot inactive) when the file is missing or malformed.
static bool ScriptLoad(uint8_t fileId) {
    if (fileId >= MAX_SCRIPTS) return false;

    char name[8];
    ScriptFileName(fileId, name);
    uint32_t size = Storage.FileExists(name);
    if (size == 0xFFFFFFFF || size < SCRIPT_HEADER_SIZE) return false;

    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) return false;
    uint32_t got = Storage.ReadFromFile(name, 0, size, (char *)buf);
    if (got < SCRIPT_HEADER_SIZE) { free(buf); return false; }

    LoadedScript *s = &scriptRegistry[fileId];
    s->Release();
    s->slot = fileId;
    s->trid = (uint16_t)(SCRIPT_TRID_BASE + fileId);

    s->properties = ScriptRdU32(buf + 0);
    s->inCount    = buf[4];
    s->outCount   = buf[5];
    s->varCount   = buf[6];
    s->constCount = buf[7];
    uint32_t constLen = ScriptRdU32(buf + 8);
    uint32_t defLen   = ScriptRdU32(buf + 12);
    s->instrLen       = ScriptRdU32(buf + 16);
    uint32_t uiLen    = ScriptRdU32(buf + 20);

    uint32_t entries = (uint32_t)(s->inCount + s->outCount + s->varCount + s->constCount);
    uint32_t need = SCRIPT_HEADER_SIZE + entries * 4 + constLen + defLen + s->instrLen + uiLen;
    if (need > got || need > size) { free(buf); return false; }

    s->inMeta    = ScriptAllocMetas(s->inCount);
    s->outMeta   = ScriptAllocMetas(s->outCount);
    s->varMeta   = ScriptAllocMetas(s->varCount);
    s->constMeta = ScriptAllocMetas(s->constCount);
    if ((s->inCount && !s->inMeta) || (s->outCount && !s->outMeta) ||
        (s->varCount && !s->varMeta) || (s->constCount && !s->constMeta)) {
        s->Release(); free(buf); return false;
    }

    uint32_t off = SCRIPT_HEADER_SIZE;
    if (s->inCount)    { memcpy(s->inMeta,    buf + off, (size_t)s->inCount * 4);    off += (uint32_t)s->inCount * 4; }
    if (s->outCount)   { memcpy(s->outMeta,   buf + off, (size_t)s->outCount * 4);   off += (uint32_t)s->outCount * 4; }
    if (s->varCount)   { memcpy(s->varMeta,   buf + off, (size_t)s->varCount * 4);   off += (uint32_t)s->varCount * 4; }
    if (s->constCount) { memcpy(s->constMeta, buf + off, (size_t)s->constCount * 4); off += (uint32_t)s->constCount * 4; }

    s->inTotal    = ScriptSumStrides(s->inMeta, s->inCount);
    s->outTotal   = ScriptSumStrides(s->outMeta, s->outCount);
    s->varTotal   = ScriptSumStrides(s->varMeta, s->varCount);
    s->constTotal = ScriptSumStrides(s->constMeta, s->constCount);

    const uint8_t *constBlob = buf + off; off += constLen;
    const uint8_t *defBlob   = buf + off; off += defLen;
    const uint8_t *instrBlob = buf + off; off += s->instrLen;
    const uint8_t *uiBlob    = buf + off; off += uiLen;

    if (s->inTotal + s->outTotal) {
        s->ioSpace = (uint8_t *)calloc(1, (size_t)s->inTotal + s->outTotal);
        if (!s->ioSpace) { s->Release(); free(buf); return false; }
    }
    s->varSpace = (uint8_t *)calloc(1, (size_t)4 + s->varTotal);
    if (!s->varSpace) { s->Release(); free(buf); return false; }
    if (s->constTotal) {
        s->constSpace = (uint8_t *)calloc(1, s->constTotal);
        if (!s->constSpace) { s->Release(); free(buf); return false; }
    }

    // Compile the program (also copies the instruction symbols into RAM).
    if (!ScriptBuildProgram(s, instrBlob, s->instrLen)) { s->Release(); free(buf); return false; }

    // Inputs carry their defaults from the file (same 4-byte-strided packing as the space).
    for (uint8_t i = 0; i < s->inCount; i++) {
        uint32_t src = 0;
        for (uint8_t k = 0; k < i; k++) src += ScriptAlign4(s->inMeta[k].Size);
        if (src + s->inMeta[i].Size <= defLen)
            memcpy(s->ioSpace + s->InputOffset(i), defBlob + src, s->inMeta[i].Size);
    }
    for (uint8_t i = 0; i < s->constCount; i++) {
        uint32_t src = 0;
        for (uint8_t k = 0; k < i; k++) src += ScriptAlign4(s->constMeta[k].Size);
        if (src + s->constMeta[i].Size <= constLen)
            memcpy(s->constSpace + s->ConstOffset(i), constBlob + src, s->constMeta[i].Size);
    }

    // UI info (version 1) starts with a length-prefixed function name; fall back to the
    // file name. The remaining names/specs are app-side editor metadata.
    s->uiLen = (uint16_t)uiLen;
    s->name[0] = '\0';
    if (uiLen >= 2 && uiBlob[0] == 1) {
        uint8_t nlen = uiBlob[1];
        if (2u + nlen <= uiLen && nlen < BLOCK_NAME_LEN) {
            memcpy(s->name, uiBlob + 2, nlen);
            s->name[nlen] = '\0';
        }
    }
    if (s->name[0] == '\0') {
        memcpy(s->name, name, 6);
        s->name[6] = '\0';
    }

    // Force the key byte to 0 and enforce read-only on outputs/constants.
    for (uint8_t i = 0; i < s->inCount; i++)    s->inMeta[i].Key = 0;
    for (uint8_t i = 0; i < s->outCount; i++)   { s->outMeta[i].Key = 0; s->outMeta[i].FlagsAndType |= FieldFlags::ReadOnly; }
    for (uint8_t i = 0; i < s->varCount; i++)   s->varMeta[i].Key = 0;
    for (uint8_t i = 0; i < s->constCount; i++) { s->constMeta[i].Key = 0; s->constMeta[i].FlagsAndType |= FieldFlags::ReadOnly; }

    s->active = true;
    s->state = (uint8_t)ScriptState::Stopped;
    s->ic = 0;
    free(buf);
    return true;
}

static void ScriptUnload(uint8_t slot) {
    LoadedScript *s = ScriptActive(slot);
    if (s) s->Release();
}

// Number of loaded scripts + their slots (dense list, stable instance = slot).
static uint8_t ScriptListInstances(uint8_t *out, uint8_t max) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_SCRIPTS && n < max; i++)
        if (scriptRegistry[i].active) out[n++] = i;
    return n;
}

static uint8_t ScriptKeyCount(uint8_t slot, uint8_t field) {
    LoadedScript *s = ScriptActive(slot);
    if (!s) return 0;
    switch (field) {
        case SCRIPT_FIELD_INPUT:    return s->inCount;
        case SCRIPT_FIELD_OUTPUT:   return s->outCount;
        default: return 0; // Header/Variables/Constants are not register content
    }
}

// Resolves one I/O entry into a descriptor + value copy. Only inputs (field 1) and
// outputs (field 2) are exposed through the Register.
static bool ScriptGetEntry(uint8_t slot, uint8_t field, uint8_t key, BlockMeta &m, uint8_t *vbuf, uint8_t &vsz) {
    LoadedScript *s = ScriptActive(slot);
    if (!s) return false;
    m = {};
    vsz = 0;

    const BlockMeta *meta = nullptr;
    const uint8_t *data = nullptr;
    if (field == SCRIPT_FIELD_INPUT && key < s->inCount) {
        meta = &s->inMeta[key]; data = s->ioSpace + s->InputOffset(key);
    } else if (field == SCRIPT_FIELD_OUTPUT && key < s->outCount) {
        meta = &s->outMeta[key]; data = s->ioSpace + s->OutputOffset(key);
    } else {
        return false;
    }

    m = *meta;
    uint8_t n = m.Size;
    if (n) memcpy(vbuf, data, n);
    vsz = n;
    return true;
}

// Writes an input (field 1) entry - the only writable Register entry of a script.
static bool ScriptSetEntry(uint8_t slot, uint8_t field, uint8_t key, const BlockMeta &m,
                           const uint8_t *val, uint16_t vlen) {
    LoadedScript *s = ScriptActive(slot);
    if (!s || field != SCRIPT_FIELD_INPUT || key >= s->inCount) return false;
    const BlockMeta *meta = &s->inMeta[key];
    uint8_t *data = s->ioSpace + s->InputOffset(key);
    if (meta->FlagsAndType & FieldFlags::ReadOnly) return false;
    if (BlockMetaType(meta->FlagsAndType) != BlockMetaType(m.FlagsAndType)) return false;
    if (vlen > meta->Size) return false;
    if (vlen) memcpy(data, val, vlen);
    return true;
}

// Writes a variable's RAM (Script management CID 7 "Write Variable" - editor debug).
static bool ScriptSetVariable(uint8_t slot, uint8_t varId, const uint8_t *val, uint16_t vlen) {
    LoadedScript *s = ScriptActive(slot);
    if (!s || varId >= s->varCount) return false;
    if (vlen > s->varMeta[varId].Size) return false;
    if (vlen) memcpy(s->varSpace + s->VarOffset(varId), val, vlen);
    return true;
}

// ===== VM =====

struct ScriptScalar {
    Number n;
    int32_t i;
};

static inline uint16_t ScriptSymVal(const uint8_t *sym) { return (uint16_t)(sym[2] | (sym[3] << 8)); }

static inline bool ScriptIsNumericDtype(uint16_t dtype) {
    switch (dtype) {
        case (uint16_t)DataType::Bool:
        case (uint16_t)DataType::Index:
        case (uint16_t)DataType::Number:
        case (uint16_t)DataType::Enum:
        case (uint16_t)DataType::Uint32:
        case (uint16_t)DataType::DevType:
        case (uint16_t)DataType::Id:
            return true;
        default:
            return false;
    }
}

static int32_t ScriptLoadInt(const uint8_t *d, uint8_t sz, bool sign) {
    int32_t v = 0;
    for (uint8_t i = 0; i < sz && i < 4; i++) v |= (int32_t)d[i] << (8 * i);
    if (sign && sz > 0 && sz < 4 && (d[sz - 1] & 0x80)) v |= ~((1 << (8 * sz)) - 1);
    return v;
}

static void ScriptStoreInt(uint8_t *d, uint8_t sz, int32_t v) {
    for (uint8_t i = 0; i < sz && i < 4; i++) d[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    for (uint8_t i = 4; i < sz; i++) d[i] = 0;
}

// Resolves a source symbol into bytes. Predefines are materialised into `scratch`.
static bool ScriptResolve(LoadedScript *s, uint8_t stype, uint8_t ssub, uint16_t sval,
                          uint8_t *scratch, const uint8_t **data, uint8_t *size, uint16_t *dtype) {
    switch (stype) {
        case SCRIPT_SYM_INPUT:
            if (sval >= s->inCount || !s->ioSpace) return false;
            *data = s->ioSpace + s->InputOffset((uint8_t)sval);
            *size = s->inMeta[sval].Size;
            *dtype = BlockMetaType(s->inMeta[sval].FlagsAndType);
            return true;
        case SCRIPT_SYM_OUTPUT:
            if (sval >= s->outCount || !s->ioSpace) return false;
            *data = s->ioSpace + s->OutputOffset((uint8_t)sval);
            *size = s->outMeta[sval].Size;
            *dtype = BlockMetaType(s->outMeta[sval].FlagsAndType);
            return true;
        case SCRIPT_SYM_VARIABLE:
            if (sval >= s->varCount) return false;
            *data = s->varSpace + s->VarOffset((uint8_t)sval);
            *size = s->varMeta[sval].Size;
            *dtype = BlockMetaType(s->varMeta[sval].FlagsAndType);
            return true;
        case SCRIPT_SYM_CONSTANT:
            if (sval >= s->constCount) return false;
            *data = s->constSpace + s->ConstOffset((uint8_t)sval);
            *size = s->constMeta[sval].Size;
            *dtype = BlockMetaType(s->constMeta[sval].FlagsAndType);
            return true;
        case SCRIPT_SYM_PREDEFINE:
            memset(scratch, 0, 4);
            switch (ssub) {
                case SCRIPT_PRE_INDEX: scratch[0] = sval & 0xFF; scratch[1] = (sval >> 8) & 0xFF; *size = 2; *dtype = (uint16_t)DataType::Index; break;
                case SCRIPT_PRE_CHAR:  scratch[0] = sval & 0xFF; *size = 1; *dtype = (uint16_t)DataType::Uint32; break;
                case SCRIPT_PRE_BOOL:  scratch[0] = sval ? 1 : 0; *size = 1; *dtype = (uint16_t)DataType::Bool; break;
                case SCRIPT_PRE_STATE: scratch[0] = sval & 0xFF; *size = 1; *dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_MATHOP:scratch[0] = sval & 0xFF; *size = 1; *dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_TYPE:  scratch[0] = sval & 0xFF; scratch[1] = (sval >> 8) & 0xFF; *size = 2; *dtype = (uint16_t)DataType::Enum; break;
                default: return false;
            }
            *data = scratch;
            return true;
        default:
            return false;
    }
}

static bool ScriptToScalar(const uint8_t *data, uint8_t size, uint16_t dtype, bool num, ScriptScalar &out) {
    out.n = Number(0);
    out.i = 0;
    if (num) {
        switch (dtype) {
            case (uint16_t)DataType::Number: {
                int32_t raw = 0; memcpy(&raw, data, 4); out.n = Number::FromRaw(raw); return true;
            }
            case (uint16_t)DataType::Index: out.n = Number(ScriptLoadInt(data, size, true)); return true;
            case (uint16_t)DataType::Uint32: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            case (uint16_t)DataType::Bool: out.n = Number(data[0] ? 1 : 0); return true;
            case (uint16_t)DataType::Enum: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            case (uint16_t)DataType::DevType: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            case (uint16_t)DataType::Id: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            default: return false;
        }
    }
    switch (dtype) {
        case (uint16_t)DataType::Number: {
            int32_t raw = 0; memcpy(&raw, data, 4); out.i = Number::FromRaw(raw).ToInt(); return true;
        }
        case (uint16_t)DataType::Index: out.i = ScriptLoadInt(data, size, true); return true;
        case (uint16_t)DataType::Uint32: out.i = ScriptLoadInt(data, size, false); return true;
        case (uint16_t)DataType::Bool: out.i = data[0] ? 1 : 0; return true;
        case (uint16_t)DataType::Enum: out.i = ScriptLoadInt(data, size, false); return true;
        case (uint16_t)DataType::DevType: out.i = ScriptLoadInt(data, size, false); return true;
        case (uint16_t)DataType::Id: out.i = ScriptLoadInt(data, size, false); return true;
        default: return false;
    }
}

static bool ScriptResolveOperandScalar(LoadedScript *s, const uint8_t *sym, bool num, ScriptScalar &out) {
    uint8_t scratch[4];
    const uint8_t *data = nullptr;
    uint8_t size = 0;
    uint16_t dtype = 0;
    if (!ScriptResolve(s, sym[0], sym[1], ScriptSymVal(sym), scratch, &data, &size, &dtype)) return false;
    return ScriptToScalar(data, size, dtype, num, out);
}

static bool ScriptResolveOperandInt(LoadedScript *s, const uint8_t *sym, int32_t &out) {
    ScriptScalar sc;
    if (!ScriptResolveOperandScalar(s, sym, false, sc)) return false;
    out = sc.i;
    return true;
}

// Resolves a destination symbol (variable or output) into its storage.
static bool ScriptResolveDest(LoadedScript *s, const uint8_t *sym, uint16_t &dtype, uint8_t *&data, uint8_t &size) {
    if (sym[0] == SCRIPT_SYM_VARIABLE) {
        uint16_t idx = ScriptSymVal(sym);
        if (idx >= s->varCount) return false;
        data = s->varSpace + s->VarOffset((uint8_t)idx);
        size = s->varMeta[idx].Size;
        dtype = BlockMetaType(s->varMeta[idx].FlagsAndType);
        return true;
    }
    if (sym[0] == SCRIPT_SYM_OUTPUT) {
        uint16_t idx = ScriptSymVal(sym);
        if (idx >= s->outCount || !s->ioSpace) return false;
        data = s->ioSpace + s->OutputOffset((uint8_t)idx);
        size = s->outMeta[idx].Size;
        dtype = BlockMetaType(s->outMeta[idx].FlagsAndType);
        return true;
    }
    return false;
}

static void ScriptStoreScalar(uint16_t dtype, uint8_t *data, uint8_t size, bool num, const ScriptScalar &v) {
    switch (dtype) {
        case (uint16_t)DataType::Number: {
            int32_t raw = num ? v.n.Value : Number(v.i).Value;
            memcpy(data, &raw, 4);
            break;
        }
        case (uint16_t)DataType::Bool: {
            int32_t iv = num ? v.n.RoundToInt() : v.i;
            data[0] = iv ? 1 : 0;
            break;
        }
        default: {
            int32_t iv = num ? v.n.RoundToInt() : v.i;
            ScriptStoreInt(data, size, iv);
            break;
        }
    }
}

// Stores a resolved source into an already-resolved destination (numeric conversion when
// both sides are numeric, raw clamped copy otherwise).
static uint8_t ScriptAssignResolved(uint16_t dtype, uint8_t *dest, uint8_t dsize,
                                    const uint8_t *src, uint8_t ssize, uint16_t stype) {
    if (ScriptIsNumericDtype(dtype) && ScriptIsNumericDtype(stype)) {
        ScriptScalar sc;
        bool num = (dtype == (uint16_t)DataType::Number);
        if (!ScriptToScalar(src, ssize, stype, num, sc)) return SCRIPT_ERR_TYPE;
        ScriptStoreScalar(dtype, dest, dsize, num, sc);
    } else {
        uint8_t n = ssize < dsize ? ssize : dsize;
        if (n) memcpy(dest, src, n);
        for (uint8_t i = n; i < dsize; i++) dest[i] = 0;
    }
    return SCRIPT_ERR_NONE;
}

static uint8_t ScriptAssign(LoadedScript *s, const uint8_t *destSym, const uint8_t *src,
                            uint8_t ssize, uint16_t stype) {
    uint16_t dtype = 0;
    uint8_t *dest = nullptr;
    uint8_t dsize = 0;
    if (!ScriptResolveDest(s, destSym, dtype, dest, dsize)) return SCRIPT_ERR_OPERAND;
    return ScriptAssignResolved(dtype, dest, dsize, src, ssize, stype);
}

// Executes a single math/logic line. Returns an error code (0 = ok).
static uint8_t ScriptExecMath(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t cat, uint8_t op) {
    if (ln.destCount < 1) return SCRIPT_ERR_OPERAND;
    uint16_t dtype = 0;
    uint8_t *dest = nullptr;
    uint8_t dsize = 0;
    if (!ScriptResolveDest(s, s->instr + (size_t)ln.start * 4, dtype, dest, dsize)) return SCRIPT_ERR_OPERAND;
    bool num = (dtype == (uint16_t)DataType::Number);

    // SET copies/assigns a single operand.
    if (cat == SCRIPT_CAT_MATH && op == SCRIPT_OP_MATH_SET) {
        if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
        uint8_t scratch[8];
        const uint8_t *src = nullptr;
        uint8_t ssize = 0;
        uint16_t stype = 0;
        if (!ScriptResolve(s, opbase[0], opbase[1], ScriptSymVal(opbase), scratch, &src, &ssize, &stype)) return SCRIPT_ERR_OPERAND;
        uint8_t err = ScriptAssignResolved(dtype, dest, dsize, src, ssize, stype);
        if (err) return err;
        s->ic++;
        return SCRIPT_ERR_NONE;
    }

    ScriptScalar in[4];
    uint8_t n = ln.opCount;
    if (n > 4) n = 4;
    for (uint8_t i = 0; i < n; i++) {
        if (!ScriptResolveOperandScalar(s, opbase + (size_t)i * 4, num, in[i])) return SCRIPT_ERR_OPERAND;
    }

    ScriptScalar out;
    out.n = Number(0);
    out.i = 0;
    switch (cat) {
        case SCRIPT_CAT_MATH:
            switch (op) {
                case SCRIPT_OP_MATH_ADD: if (num) out.n = in[0].n + in[1].n; else out.i = in[0].i + in[1].i; break;
                case SCRIPT_OP_MATH_SUB: if (num) out.n = in[0].n - in[1].n; else out.i = in[0].i - in[1].i; break;
                case SCRIPT_OP_MATH_MUL: if (num) out.n = in[0].n * in[1].n; else out.i = in[0].i * in[1].i; break;
                case SCRIPT_OP_MATH_DIV:
                    if (num) out.n = in[0].n / in[1].n;
                    else { if (in[1].i == 0) return SCRIPT_ERR_TYPE; out.i = in[0].i / in[1].i; }
                    break;
                case SCRIPT_OP_MATH_MOD:
                    if (num) { int32_t b = in[1].n.RoundToInt(); if (b == 0) return SCRIPT_ERR_TYPE; out.n = Number(in[0].n.RoundToInt() % b); }
                    else { if (in[1].i == 0) return SCRIPT_ERR_TYPE; out.i = in[0].i % in[1].i; }
                    break;
                case SCRIPT_OP_MATH_MIN: if (num) out.n = min(in[0].n, in[1].n); else out.i = in[0].i < in[1].i ? in[0].i : in[1].i; break;
                case SCRIPT_OP_MATH_MAX: if (num) out.n = max(in[0].n, in[1].n); else out.i = in[0].i > in[1].i ? in[0].i : in[1].i; break;
                case SCRIPT_OP_MATH_NEG: if (num) out.n = -in[0].n; else out.i = -in[0].i; break;
                case SCRIPT_OP_MATH_ABS: if (num) out.n = abs(in[0].n); else out.i = in[0].i < 0 ? -in[0].i : in[0].i; break;
                default: return SCRIPT_ERR_UNKNOWN_OP;
            }
            break;
        case SCRIPT_CAT_LOGIC:
            switch (op) {
                case SCRIPT_OP_LOGIC_AND: if (num) out.n = Number((in[0].n.Value != 0) && (in[1].n.Value != 0)); else out.i = in[0].i & in[1].i; break;
                case SCRIPT_OP_LOGIC_OR:  if (num) out.n = Number((in[0].n.Value != 0) || (in[1].n.Value != 0)); else out.i = in[0].i | in[1].i; break;
                case SCRIPT_OP_LOGIC_XOR: if (num) out.n = Number((in[0].n.Value != 0) != (in[1].n.Value != 0)); else out.i = in[0].i ^ in[1].i; break;
                case SCRIPT_OP_LOGIC_NOT: if (num) out.n = Number(in[0].n.Value == 0); else out.i = ~in[0].i; break;
                case SCRIPT_OP_LOGIC_SHL: { int32_t a = num ? in[0].n.RoundToInt() : in[0].i; int32_t b = num ? in[1].n.RoundToInt() : in[1].i; int32_t r = (uint32_t)a << (b & 31); if (num) out.n = Number(r); else out.i = r; break; }
                case SCRIPT_OP_LOGIC_SHR: { int32_t a = num ? in[0].n.RoundToInt() : in[0].i; int32_t b = num ? in[1].n.RoundToInt() : in[1].i; int32_t r = a >> (b & 31); if (num) out.n = Number(r); else out.i = r; break; }
                case SCRIPT_OP_LOGIC_CMP_EQ: case SCRIPT_OP_LOGIC_CMP_NE:
                case SCRIPT_OP_LOGIC_CMP_LT: case SCRIPT_OP_LOGIC_CMP_LE:
                case SCRIPT_OP_LOGIC_CMP_GT: case SCRIPT_OP_LOGIC_CMP_GE: {
                    bool r;
                    if (num) { Number a = in[0].n, b = in[1].n;
                        r = (op == SCRIPT_OP_LOGIC_CMP_EQ) ? (a.Value == b.Value) : (op == SCRIPT_OP_LOGIC_CMP_NE) ? (a.Value != b.Value) :
                            (op == SCRIPT_OP_LOGIC_CMP_LT) ? (a.Value < b.Value) : (op == SCRIPT_OP_LOGIC_CMP_LE) ? (a.Value <= b.Value) :
                            (op == SCRIPT_OP_LOGIC_CMP_GT) ? (a.Value > b.Value) : (a.Value >= b.Value);
                    } else { int32_t a = in[0].i, b = in[1].i;
                        r = (op == SCRIPT_OP_LOGIC_CMP_EQ) ? (a == b) : (op == SCRIPT_OP_LOGIC_CMP_NE) ? (a != b) :
                            (op == SCRIPT_OP_LOGIC_CMP_LT) ? (a < b) : (op == SCRIPT_OP_LOGIC_CMP_LE) ? (a <= b) :
                            (op == SCRIPT_OP_LOGIC_CMP_GT) ? (a > b) : (a >= b);
                    }
                    if (num) out.n = Number(r ? 1 : 0); else out.i = r ? 1 : 0;
                    break;
                }
                case SCRIPT_OP_LOGIC_SELECT: {
                    if (n < 3) return SCRIPT_ERR_OPERAND;
                    bool cond = num ? (in[0].n.Value != 0) : (in[0].i != 0);
                    if (num) out.n = cond ? in[1].n : in[2].n; else out.i = cond ? in[1].i : in[2].i;
                    break;
                }
                default: return SCRIPT_ERR_UNKNOWN_OP;
            }
            break;
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }

    ScriptStoreScalar(dtype, dest, dsize, num, out);
    s->ic++;
    return SCRIPT_ERR_NONE;
}

// Evaluates an operand as a truth value.
static bool ScriptOperandTruthy(LoadedScript *s, const uint8_t *sym, bool &ok) {
    uint8_t scratch[4];
    const uint8_t *data = nullptr;
    uint8_t size = 0;
    uint16_t dtype = 0;
    ok = false;
    if (!ScriptResolve(s, sym[0], sym[1], ScriptSymVal(sym), scratch, &data, &size, &dtype)) return false;
    ok = ScriptIsNumericDtype(dtype) && ScriptLoadInt(data, size, dtype == (uint16_t)DataType::Index) != 0;
    return true;
}

static uint8_t ScriptExecFlow(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t op) {
    switch (op) {
        case SCRIPT_OP_FLOW_IF: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool ok = false;
            if (!ScriptOperandTruthy(s, opbase, ok)) return SCRIPT_ERR_OPERAND;
            uint16_t match = s->blockMatch[s->ic];
            s->ic = ok ? s->ic + 1 : (match == 0xFFFF ? s->ic + 1 : match + 1);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_WHILE: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool ok = false;
            if (!ScriptOperandTruthy(s, opbase, ok)) return SCRIPT_ERR_OPERAND;
            uint16_t match = s->blockMatch[s->ic];
            s->ic = ok ? s->ic + 1 : (match == 0xFFFF ? s->ic + 1 : match + 1);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_END: {
            uint16_t open = (s->blockMatch[s->ic] == 0xFFFF) ? 0xFFFF : s->blockMatch[s->ic];
            if (open != 0xFFFF && s->blockKind[open] == 2) { s->ic = open; return SCRIPT_ERR_NONE; } // while -> re-check
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_JUMP: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t target = 0;
            if (!ScriptResolveOperandInt(s, opbase, target)) return SCRIPT_ERR_OPERAND;
            if (target < 0 || target >= s->lineCount) return SCRIPT_ERR_BOUNDS;
            s->ic = (uint16_t)target;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_CALL: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t target = 0;
            if (!ScriptResolveOperandInt(s, opbase, target)) return SCRIPT_ERR_OPERAND;
            if (target < 0 || target >= s->lineCount) return SCRIPT_ERR_BOUNDS;
            if (s->callDepth >= SCRIPT_MAX_CALL_DEPTH) return SCRIPT_ERR_STACK;
            s->callStack[s->callDepth++] = (uint16_t)(s->ic + 1);
            s->ic = (uint16_t)target;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_RETURN: {
            if (s->callDepth == 0) { s->state = (uint8_t)ScriptState::Finished; return SCRIPT_ERR_NONE; }
            s->ic = s->callStack[--s->callDepth];
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_HALT:
            s->state = (uint8_t)ScriptState::Finished;
            return SCRIPT_ERR_NONE;
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}

static uint8_t ScriptExecTime(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t op, uint32_t nowMs, bool &yield) {
    switch (op) {
        case SCRIPT_OP_TIME_DELAY: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t ms = 0;
            if (!ScriptResolveOperandInt(s, opbase, ms)) return SCRIPT_ERR_OPERAND;
            if (ms < 0) ms = 0;
            s->ic++;
            s->waitUntil = nowMs + (uint32_t)ms;
            s->waitingOnTime = true;
            s->state = (uint8_t)ScriptState::Waiting;
            yield = true;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_TIME_WAIT: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool ok = false;
            if (!ScriptOperandTruthy(s, opbase, ok)) return SCRIPT_ERR_OPERAND;
            if (!ok) {
                s->waitingOnTime = false;
                s->state = (uint8_t)ScriptState::Waiting;
                yield = true; // ic stays: re-evaluated on resume
                return SCRIPT_ERR_NONE;
            }
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_TIME_GET: {
            if (ln.destCount < 1) return SCRIPT_ERR_OPERAND;
            ScriptScalar v;
            v.i = (int32_t)nowMs;
            v.n = Number((int32_t)nowMs);
            uint16_t dtype = 0; uint8_t *dest = nullptr; uint8_t dsize = 0;
            if (!ScriptResolveDest(s, s->instr + (size_t)ln.start * 4, dtype, dest, dsize)) return SCRIPT_ERR_OPERAND;
            ScriptStoreScalar(dtype, dest, dsize, dtype == (uint16_t)DataType::Number, v);
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}

// Reads a 4-byte BlockInfo from a Constant operand.
static bool ScriptOperandBlockInfo(LoadedScript *s, const uint8_t *sym, uint32_t &bi) {
    uint8_t scratch[4];
    const uint8_t *data = nullptr;
    uint8_t size = 0;
    uint16_t dtype = 0;
    if (!ScriptResolve(s, sym[0], sym[1], ScriptSymVal(sym), scratch, &data, &size, &dtype)) return false;
    if (size < 4) return false;
    memcpy(&bi, data, 4);
    return true;
}

static void ScriptSendRegisterRequest(LoadedScript *s, uint16_t addr, uint8_t cid,
                                      const uint8_t *payload, uint16_t len) {
    PacketFrame req;
    PacketConstruct(&req, addr, MakeService(ServiceType::Register, cid), s->trid,
                    FLAG_REQACK | FLAG_START | FLAG_STOP, payload, len);
    SendAndVerifyPacket(req);
}

static uint8_t ScriptExecService(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                                 uint8_t op, uint32_t nowMs, bool &yield) {
    switch (op) {
        case SCRIPT_OP_SERVICE_NOP:
            s->ic++;
            return SCRIPT_ERR_NONE;
        case SCRIPT_OP_SERVICE_LOG:
            s->ic++;
            return SCRIPT_ERR_NONE;
        case SCRIPT_OP_SERVICE_STATE: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            if (opbase[0] == SCRIPT_SYM_PREDEFINE && opbase[1] == SCRIPT_PRE_STATE) {
                uint8_t st = (uint8_t)(ScriptSymVal(opbase) & 0xFF);
                if (st > (uint8_t)ScriptState::Error) return SCRIPT_ERR_OPERAND;
                s->state = st;
                s->ic++;
                return SCRIPT_ERR_NONE;
            }
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_READ: { // dest = register[BlockInfo]
            if (ln.destCount < 1 || ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            uint32_t bi = 0;
            if (!ScriptOperandBlockInfo(s, opbase, bi)) return SCRIPT_ERR_OPERAND;
            BlockMeta rm;
            uint8_t rbuf[64];
            uint8_t rsz = 0;
            if (!RegisterGetByBlockInfo(bi, rm, rbuf, rsz)) return SCRIPT_ERR_REGISTER;
            uint8_t err = ScriptAssign(s, s->instr + (size_t)ln.start * 4, rbuf, rsz,
                                       BlockMetaType(rm.FlagsAndType));
            if (err) return err;
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_WRITE: { // register[BlockInfo] = operand2
            if (ln.opCount < 2) return SCRIPT_ERR_OPERAND;
            uint32_t bi = 0;
            if (!ScriptOperandBlockInfo(s, opbase, bi)) return SCRIPT_ERR_OPERAND;
            BlockMeta rm;
            uint8_t rbuf[64];
            uint8_t rsz = 0;
            if (!RegisterGetByBlockInfo(bi, rm, rbuf, rsz)) return SCRIPT_ERR_REGISTER;
            uint8_t scratch[4];
            const uint8_t *val = nullptr;
            uint8_t vsize = 0;
            uint16_t vtype = 0;
            if (!ScriptResolve(s, opbase[4], opbase[5], ScriptSymVal(opbase + 4), scratch, &val, &vsize, &vtype))
                return SCRIPT_ERR_OPERAND;
            uint8_t wbuf[64];
            memset(wbuf, 0, sizeof(wbuf));
            uint8_t err = ScriptAssignResolved(BlockMetaType(rm.FlagsAndType), wbuf, rm.Size, val, vsize, vtype);
            if (err) return err;
            BlockMeta wm = rm;
            wm.FlagsAndType = (uint16_t)(rm.FlagsAndType | FieldFlags::ScriptUpdated);
            wm.Size = rm.Size;
            if (!RegisterSetByBlockInfo(bi, wm, wbuf, rm.Size)) return SCRIPT_ERR_REGISTER;
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_READ_FOREIGN: { // dest = register[addr, BlockInfo]
            if (ln.destCount < 1 || ln.opCount < 2) return SCRIPT_ERR_OPERAND;
            int32_t addr = 0;
            if (!ScriptResolveOperandInt(s, opbase, addr)) return SCRIPT_ERR_OPERAND;
            uint32_t bi = 0;
            if (!ScriptOperandBlockInfo(s, opbase + 4, bi)) return SCRIPT_ERR_OPERAND;
            memcpy(s->pendingDest, s->instr + (size_t)ln.start * 4, 4);
            s->pendingRead = true;
            s->pendingForeign = true;
            s->pendingTrid = s->trid;
            s->pendingDeadline = nowMs + 500;
            s->ic++;
            s->state = (uint8_t)ScriptState::Waiting;
            s->waitingOnTime = false;
            yield = true;
            uint8_t payload[4];
            memcpy(payload, &bi, 4);
            ScriptSendRegisterRequest(s, (uint16_t)addr, 1, payload, 4);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_WRITE_FOREIGN: { // register[addr, BlockInfo] = operand3
            if (ln.opCount < 3) return SCRIPT_ERR_OPERAND;
            int32_t addr = 0;
            if (!ScriptResolveOperandInt(s, opbase, addr)) return SCRIPT_ERR_OPERAND;
            uint32_t bi = 0;
            if (!ScriptOperandBlockInfo(s, opbase + 4, bi)) return SCRIPT_ERR_OPERAND;
            uint8_t scratch[4];
            const uint8_t *val = nullptr;
            uint8_t vsize = 0;
            uint16_t vtype = 0;
            if (!ScriptResolve(s, opbase[8], opbase[9], ScriptSymVal(opbase + 8), scratch, &val, &vsize, &vtype))
                return SCRIPT_ERR_OPERAND;
            if (vsize > 64) vsize = 64;
            uint8_t payload[8 + 64];
            memcpy(payload, &bi, 4);
            BlockMeta wm;
            wm.FlagsAndType = vtype;
            wm.Key = 0;
            wm.Size = vsize;
            memcpy(payload + 4, &wm, 4);
            if (vsize) memcpy(payload + 8, val, vsize);
            s->pendingRead = false;
            s->pendingForeign = true;
            s->pendingTrid = s->trid;
            s->pendingDeadline = nowMs + 500;
            s->ic++;
            s->state = (uint8_t)ScriptState::Waiting;
            s->waitingOnTime = false;
            yield = true;
            ScriptSendRegisterRequest(s, (uint16_t)addr, 2, payload, (uint16_t)(8 + vsize));
            return SCRIPT_ERR_NONE;
        }
        default:
            return SCRIPT_ERR_NOT_IMPL;
    }
}

// Resolves one element of a vector/matrix/colour/string container.
static bool ScriptContainerElement(LoadedScript *s, const uint8_t *sym, int32_t index,
                                   uint8_t *scratch, const uint8_t **elem, uint8_t *elemSize,
                                   uint16_t *elemType) {
    const uint8_t *data = nullptr;
    uint8_t size = 0;
    uint16_t dtype = 0;
    if (!ScriptResolve(s, sym[0], sym[1], ScriptSymVal(sym), scratch, &data, &size, &dtype)) return false;
    switch (dtype) {
        case (uint16_t)DataType::Vector:
            if (index < 0 || (uint32_t)(index + 1) * 4 > size) return false;
            *elem = data + (size_t)index * 4;
            *elemSize = 4;
            *elemType = (uint16_t)DataType::Number;
            return true;
        case (uint16_t)DataType::Matrix: {
            if (size < 4 || index < 0) return false;
            uint16_t h = (uint16_t)(data[0] | (data[1] << 8));
            uint16_t w = (uint16_t)(data[2] | (data[3] << 8));
            if ((uint32_t)index >= (uint32_t)h * w) return false;
            if (4u + (uint32_t)(index + 1) * 4 > size) return false;
            *elem = data + 4 + (size_t)index * 4;
            *elemSize = 4;
            *elemType = (uint16_t)DataType::Number;
            return true;
        }
        case (uint16_t)DataType::Colour:
            if (index < 0 || index > 3 || size < 4) return false;
            *elem = data + index;
            *elemSize = 1;
            *elemType = (uint16_t)DataType::Uint32;
            return true;
        case (uint16_t)DataType::String:
            if (index < 0 || index >= size) return false;
            *elem = data + index;
            *elemSize = 1;
            *elemType = (uint16_t)DataType::Uint32;
            return true;
        default:
            if (index != 0) return false;
            *elem = data;
            *elemSize = size;
            *elemType = dtype;
            return true;
    }
}

// Compose: container[index...] = value...   Extract: dest = container[index]
static uint8_t ScriptExecCompose(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t op) {
    if (op == SCRIPT_OP_COMPOSE) {
        if (ln.destCount < 1 || ln.opCount < 2) return SCRIPT_ERR_OPERAND;
        const uint8_t *destSym = s->instr + (size_t)ln.start * 4;
        int32_t index = 0;
        if (!ScriptResolveOperandInt(s, opbase, index)) return SCRIPT_ERR_OPERAND;
        for (uint8_t k = 1; k < ln.opCount; k++) {
            uint8_t scratch[4];
            const uint8_t *val = nullptr;
            uint8_t vsize = 0;
            uint16_t vtype = 0;
            if (!ScriptResolve(s, opbase[k * 4], opbase[k * 4 + 1], ScriptSymVal(opbase + (size_t)k * 4),
                               scratch, &val, &vsize, &vtype))
                return SCRIPT_ERR_OPERAND;
            const uint8_t *elem = nullptr;
            uint8_t esize = 0;
            uint16_t etype = 0;
            if (!ScriptContainerElement(s, destSym, index + (k - 1), scratch, &elem, &esize, &etype))
                return SCRIPT_ERR_BOUNDS;
            uint8_t err = ScriptAssignResolved(etype, const_cast<uint8_t *>(elem), esize, val, vsize, vtype);
            if (err) return err;
        }
        s->ic++;
        return SCRIPT_ERR_NONE;
    }
    if (op == SCRIPT_OP_EXTRACT) {
        if (ln.destCount < 1 || ln.opCount < 2) return SCRIPT_ERR_OPERAND;
        int32_t index = 0;
        if (!ScriptResolveOperandInt(s, opbase + 4, index)) return SCRIPT_ERR_OPERAND;
        uint8_t scratch[4];
        const uint8_t *elem = nullptr;
        uint8_t esize = 0;
        uint16_t etype = 0;
        if (!ScriptContainerElement(s, opbase, index, scratch, &elem, &esize, &etype)) return SCRIPT_ERR_BOUNDS;
        uint8_t err = ScriptAssign(s, s->instr + (size_t)ln.start * 4, elem, esize, etype);
        if (err) return err;
        s->ic++;
        return SCRIPT_ERR_NONE;
    }
    return SCRIPT_ERR_UNKNOWN_OP;
}

static uint8_t ScriptExecLine(LoadedScript *s, uint32_t nowMs, bool &yield) {
    const ScriptLineInfo &ln = s->lines[s->ic];
    const uint8_t *instr = s->instr + (size_t)(ln.start + ln.destCount) * 4;
    uint8_t cat = instr[1];
    uint8_t op = instr[2];
    const uint8_t *opbase = instr + 4;
    switch (cat) {
        case SCRIPT_CAT_MATH:
        case SCRIPT_CAT_LOGIC:
            return ScriptExecMath(s, ln, opbase, cat, op);
        case SCRIPT_CAT_FLOW:
            return ScriptExecFlow(s, ln, opbase, op);
        case SCRIPT_CAT_TIME:
            return ScriptExecTime(s, ln, opbase, op, nowMs, yield);
        case SCRIPT_CAT_SERVICE:
            return ScriptExecService(s, ln, opbase, op, nowMs, yield);
        case SCRIPT_CAT_COMPOSE:
            return ScriptExecCompose(s, ln, opbase, op);
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}

static uint32_t scriptTick = 0;

static void ScriptRun(LoadedScript *s, uint32_t nowMs) {
    uint16_t steps = 0;
    while (steps++ < SCRIPT_INSTR_BUDGET) {
        if (s->lineCount == 0 || s->ic >= s->lineCount) { s->state = (uint8_t)ScriptState::Finished; return; }
        if (s->lineStamp && s->lineStamp[s->ic] == scriptTick) return; // looped this tick -> yield
        if (s->lineStamp) s->lineStamp[s->ic] = scriptTick;
        bool yield = false;
        uint8_t err = ScriptExecLine(s, nowMs, yield);
        if (err) { s->errorCode = err; s->state = (uint8_t)ScriptState::Error; return; }
        if (yield) return;
        if (s->state != (uint8_t)ScriptState::Running) return; // Finished/Error/Paused set by the op
    }
}

void ScriptsTick(uint32_t nowMs) {
    scriptTick++;
    for (uint8_t i = 0; i < MAX_SCRIPTS; i++) {
        LoadedScript *s = &scriptRegistry[i];
        if (!s->active) continue;
        if (s->state == (uint8_t)ScriptState::Running) {
            ScriptRun(s, nowMs);
        } else if (s->state == (uint8_t)ScriptState::Waiting) {
            if (s->pendingForeign) {
                if ((int32_t)(nowMs - s->pendingDeadline) >= 0) {
                    s->pendingForeign = false;
                    s->errorCode = SCRIPT_ERR_TIMEOUT;
                    s->state = (uint8_t)ScriptState::Error;
                }
                continue;
            }
            if (s->waitingOnTime && (int32_t)(nowMs - s->waitUntil) >= 0) {
                s->waitingOnTime = false;
                s->state = (uint8_t)ScriptState::Running;
            } else if (!s->waitingOnTime) {
                s->state = (uint8_t)ScriptState::Running; // WaitUntil: re-evaluate
            }
        }
    }
}

// Handles a reply to a foreign register request. The reply's CMD carries the script's
// TRID, so the Dispatcher routes it here by range instead of by ServiceType.
static void HandleScriptResponse(const PacketFrame &frame) {
    uint16_t trid = frame.srv_tgt;
    for (uint8_t i = 0; i < MAX_SCRIPTS; i++) {
        LoadedScript *s = &scriptRegistry[i];
        if (!s->active || !s->pendingForeign || s->pendingTrid != trid) continue;
        s->pendingForeign = false;
        if (s->pendingRead) {
            uint16_t pb = PayloadBytes(frame);
            if (pb < 8) {
                s->errorCode = SCRIPT_ERR_REGISTER;
                s->state = (uint8_t)ScriptState::Error;
                return;
            }
            BlockMeta rm;
            memcpy(&rm, frame.payload + 4, 4);
            const uint8_t *val = frame.payload + 8;
            uint8_t avail = (uint8_t)(pb - 8);
            uint16_t vsz = rm.Size;
            if (vsz > avail) vsz = avail;
            uint16_t dtype = 0;
            uint8_t *dest = nullptr;
            uint8_t dsize = 0;
            if (ScriptResolveDest(s, s->pendingDest, dtype, dest, dsize))
                ScriptAssignResolved(dtype, dest, dsize, val, (uint8_t)vsz, BlockMetaType(rm.FlagsAndType));
        }
        s->state = (uint8_t)ScriptState::Running;
        return;
    }
}

static void ScriptSetState(LoadedScript *s, uint8_t newState) {
    if (newState == (uint8_t)ScriptState::Running) {
        if (s->state == (uint8_t)ScriptState::Stopped || s->state == (uint8_t)ScriptState::Finished ||
            s->state == (uint8_t)ScriptState::Error) {
            s->ic = 0;
            s->callDepth = 0;
        }
        s->waitingOnTime = false;
        s->errorCode = SCRIPT_ERR_NONE;
    } else if (newState == (uint8_t)ScriptState::Stopped) {
        s->ic = 0;
        s->callDepth = 0;
        s->waitingOnTime = false;
        s->errorCode = SCRIPT_ERR_NONE;
    }
    s->state = newState;
}

// Boot: load every stored script flagged Load-on-boot; run those flagged Run-on-load.
void ScriptsBootLoad() {
    for (uint8_t i = 0; i < MAX_SCRIPTS; i++) {
        char name[8];
        ScriptFileName(i, name);
        if (Storage.FileExists(name) == 0xFFFFFFFF) continue;
        if (ScriptLoad(i) && (scriptRegistry[i].properties & SCRIPT_PROP_RUN_ON_LOAD)) {
            scriptRegistry[i].state = (uint8_t)ScriptState::Running;
        }
    }
}

// ===== Management commands (0x050X) =====

static void ScriptReply(const PacketFrame &frame, const uint8_t *payload, uint16_t len) {
    SendResponse(frame, payload, len);
}

__attribute__((noinline)) static void HandleScript(const PacketFrame &frame) {
    if (frame.flags & FLAG_TYPE) return; // responses are not handled locally

    uint8_t cid = GetServiceCID(frame.srv_tgt);
    uint16_t bytes = PayloadBytes(frame);

    switch (cid) {
        case 0: { // Get currently loaded scripts
            uint8_t buf[1 + MAX_SCRIPTS];
            uint8_t n = ScriptListInstances(buf + 1, MAX_SCRIPTS);
            buf[0] = n;
            ScriptReply(frame, buf, 1 + n);
            break;
        }

        case 1: { // Load Script (File ID -> loaded ID)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            uint8_t fileId = frame.payload[0];
            bool ok = ScriptLoad(fileId);
            uint8_t reply = ok ? fileId : 0xFF;
            ScriptReply(frame, &reply, 1);
            break;
        }

        case 2: { // Unload script (loaded ID)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            uint8_t slot = frame.payload[0];
            bool ok = ScriptActive(slot) != nullptr;
            if (ok) ScriptUnload(slot);
            RespondStatus(frame, ok);
            break;
        }

        case 3: { // Read state (loaded ID -> state)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s) { RespondStatus(frame, false); return; }
            ScriptReply(frame, &s->state, 1);
            break;
        }

        case 4: { // Set state (loaded ID, new state)
            if (bytes < 2) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s || frame.payload[1] > (uint8_t)ScriptState::Error) { RespondStatus(frame, false); return; }
            ScriptSetState(s, frame.payload[1]);
            RespondStatus(frame, true);
            break;
        }

        case 5: { // Read internal state (loaded ID -> instruction counter (line) + variable RAM)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s) { RespondStatus(frame, false); return; }
            uint8_t buf[MAX_PAYLOAD_SIZE];
            uint32_t ic = s->ic;
            memcpy(buf, &ic, 4);
            uint16_t n = s->varTotal;
            if (n > MAX_PAYLOAD_SIZE - 4) n = MAX_PAYLOAD_SIZE - 4;
            if (n) memcpy(buf + 4, s->varSpace + 4, n);
            ScriptReply(frame, buf, 4 + n);
            break;
        }

        case 6: { // Move to instruction (loaded ID, line index u32)
            if (bytes < 5) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s) { RespondStatus(frame, false); return; }
            uint32_t line = 0;
            memcpy(&line, frame.payload + 1, 4);
            if (s->lineCount && line >= s->lineCount) line = s->lineCount - 1;
            s->ic = (uint16_t)line;
            RespondStatus(frame, true);
            break;
        }

        case 7: { // Write Variable (loaded ID, variable ID, value)
            if (bytes < 2) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            uint8_t varId = frame.payload[1];
            if (!s || varId >= s->varCount) { RespondStatus(frame, false); return; }
            // The wire payload is 4-byte padded, so the value length comes from the
            // variable's declared size, not from the remaining payload bytes.
            uint16_t vlen = s->varMeta[varId].Size;
            if ((uint16_t)(2 + vlen) > bytes) { RespondStatus(frame, false); return; }
            RespondStatus(frame, ScriptSetVariable(frame.payload[0], varId, frame.payload + 2, vlen));
            break;
        }

        case 8: { // Read error (loaded ID -> error code)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s) { RespondStatus(frame, false); return; }
            ScriptReply(frame, &s->errorCode, 1);
            break;
        }

        default:
            RespondStatus(frame, false);
            break;
    }
}

#endif // USE_SCRIPTS
