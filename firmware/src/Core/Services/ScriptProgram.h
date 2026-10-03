#pragma once

// SCR_XXX file parsing and program compilation (ScriptProgram.h) - part of the script service.
//
// Turns a script file into instruction/line tables and owns load/unload and
// the Register-facing entry accessors.
//
// Split out of Script.h; included by it in order so the whole service stays one
// translation unit. Wrapped in the same guard so it is a no-op without USE_SCRIPTS.

#ifdef USE_SCRIPTS

#include "Core/Services/ScriptDefs.h"
#include <cstdlib>
#include <cstring>

static ValueInfo *ScriptAllocMetas(uint8_t count) {
    if (count == 0) return nullptr;
    return (ValueInfo *)calloc(count, sizeof(ValueInfo));
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

// Parses SCR_<fileId> into registry `slot`. Docs/Services/Script.md: the loaded-script table
// maps a loaded id (the slot) to a file id, and the caller picks the slot - so the two need not
// be equal. The file id space is wider than the slot space (SCR_XXX vs the 6-bit instance), so
// the app stores many files and loads them into free slots. Returns false (and leaves the slot
// inactive) when the file is missing, malformed, or the slot/file id is out of range.
static bool ScriptLoad(uint16_t fileId, uint16_t slot) {
    if (fileId >= MAX_SCRIPT_FILES || slot >= MAX_SCRIPTS) return false;

    char name[8];
    ScriptFileName(fileId, name);
    uint32_t size = Storage.FileExists(name);
    if (size == 0xFFFFFFFF || size < SCRIPT_HEADER_SIZE) return false;

    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) return false;
    uint32_t got = Storage.ReadFromFile(name, 0, size, (char *)buf);
    if (got < SCRIPT_HEADER_SIZE) { free(buf); return false; }

    LoadedScript *s = &scriptRegistry[slot];
    s->Release();
    ScriptMaskSet(slot, false);
    s->slot = slot;
    s->fileId = fileId;
    s->trid = (uint16_t)(SCRIPT_TRID_BASE + slot);

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

    // Prefix-sum the strides once: symbol resolution then costs a table lookup instead of a
    // walk over the preceding entries (the VM does that per operand per line per tick).
    if (!s->BuildOffsets()) { s->Release(); free(buf); return false; }

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

    // Inputs carry their defaults from the file (same 4-byte-strided packing as the space,
    // so the file's offset is the space's own offset - no need to re-walk the strides).
    for (uint8_t i = 0; i < s->inCount; i++) {
        uint32_t src = s->InputOffset(i);
        if (src + s->inMeta[i].Size <= defLen)
            memcpy(s->ioSpace + src, defBlob + src, s->inMeta[i].Size);
    }
    for (uint8_t i = 0; i < s->constCount; i++) {
        uint32_t src = s->ConstOffset(i);
        if (src + s->constMeta[i].Size <= constLen)
            memcpy(s->constSpace + src, constBlob + src, s->constMeta[i].Size);
    }

    // UI info (version 1) starts with a length-prefixed function name; fall back to the
    // file name. The remaining names/specs are app-side editor metadata.
    s->uiLen = (uint16_t)uiLen;
    s->name[0] = '\0';
    // UI info v2 starts with the version byte then the function name; the later sections
    // (enum option labels) are app-side only and ignored here. v1 is no longer supported.
    if (uiLen >= 2 && uiBlob[0] == 2) {
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
    for (uint8_t i = 0; i < s->outCount; i++)   s->outMeta[i].Flags |= ValueReadOnly;
    for (uint8_t i = 0; i < s->constCount; i++) s->constMeta[i].Flags |= ValueReadOnly;

    s->active = true;
    ScriptMaskSet(slot, true);
    s->state = (uint8_t)ScriptState::Stopped;
    s->ic = 0;
    free(buf);
    return true;
}

static void ScriptUnload(uint16_t slot) {
    LoadedScript *s = ScriptActive(slot);
    if (!s) return;
    s->Release();
    ScriptMaskSet(slot, false);
}

// The loaded scripts' file ids (dense list, in slot order), each as two little-endian bytes.
static uint16_t ScriptListFiles(uint8_t *out, uint16_t max) {
    uint8_t n = 0;
    for (uint16_t i = 0; i < MAX_SCRIPTS && n < max; i++)
        if (scriptRegistry[i].active) {
            out[2 * n] = (uint8_t)(scriptRegistry[i].fileId & 0xFF);
            out[2 * n + 1] = (uint8_t)(scriptRegistry[i].fileId >> 8);
            n++;
        }
    return n;
}

static uint8_t ScriptKeyCount(uint16_t slot, uint8_t field) {
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
static bool ScriptGetEntry(uint16_t slot, uint8_t field, uint8_t key, ValueInfo &m, uint8_t *vbuf, uint8_t &vsz) {
    LoadedScript *s = ScriptActive(slot);
    if (!s) return false;
    m = {};
    vsz = 0;

    const ValueInfo *meta = nullptr;
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
static bool ScriptSetEntry(uint16_t slot, uint8_t field, uint8_t key, const ValueInfo &m,
                           const uint8_t *val, uint16_t vlen) {
    LoadedScript *s = ScriptActive(slot);
    if (!s || field != SCRIPT_FIELD_INPUT || key >= s->inCount) return false;
    const ValueInfo *meta = &s->inMeta[key];
    uint8_t *data = s->ioSpace + s->InputOffset(key);
    if (ValueIsReadOnly(*meta)) return false;
    if (ValueInfoType(*meta) != ValueInfoType(m)) return false;
    if (vlen > meta->Size) return false;
    if (vlen) memcpy(data, val, vlen);
    // A short write defines the rest of the fixed-size input too: spaces for a string
    // (matching the static-block behaviour), zero otherwise, so no stale bytes survive.
    uint16_t type = ValueInfoType(*meta);
    uint8_t fill = (type == (uint16_t)DataType::String || type == (uint16_t)DataType::Filename)
                       ? (uint8_t)' ' : 0;
    for (uint16_t i = vlen; i < meta->Size; i++) data[i] = fill;
    return true;
}

// Non-copying I/O lookup for subscription sources and cross-service register access:
// returns a pointer straight into the I/O space (inputs then outputs).
static bool ScriptGetIoPointer(uint16_t slot, uint8_t field, uint8_t key, ValueInfo &m, void *&data) {
    LoadedScript *s = ScriptActive(slot);
    if (!s) return false;
    if (field == SCRIPT_FIELD_INPUT && key < s->inCount) {
        m = s->inMeta[key];
        data = s->ioSpace + s->InputOffset(key);
        return true;
    }
    if (field == SCRIPT_FIELD_OUTPUT && key < s->outCount) {
        m = s->outMeta[key];
        data = s->ioSpace + s->OutputOffset(key);
        return true;
    }
    return false;
}

// Writes a variable's RAM (Script management CID 7 "Write Variable" - editor debug).
static bool ScriptSetVariable(uint16_t slot, uint8_t varId, const uint8_t *val, uint16_t vlen) {
    LoadedScript *s = ScriptActive(slot);
    if (!s || varId >= s->varCount) return false;
    if (vlen > s->varMeta[varId].Size) return false;
    if (vlen) memcpy(s->varSpace + s->VarOffset(varId), val, vlen);
    return true;
}


#endif // USE_SCRIPTS
