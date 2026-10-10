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

// SCR_XXX (8-char space-padded name, the file id in three hex digits).
inline void ScriptService::FileName(uint16_t Id, char Out[8]) {
    static const char Hex[] = "0123456789ABCDEF";
    Out[0] = 'S'; Out[1] = 'C'; Out[2] = 'R'; Out[3] = '_';
    Out[4] = Hex[(Id >> 8) & 0x0F];
    Out[5] = Hex[(Id >> 4) & 0x0F];
    Out[6] = Hex[Id & 0x0F];
    Out[7] = ' ';
}

// Parses a `SCR_XXX` file name back to its id, or 0xFFFF when it is not a script file.
inline uint16_t ScriptService::FileIdFromName(const char Name[8]) {
    if (Name[0] != 'S' || Name[1] != 'C' || Name[2] != 'R' || Name[3] != '_') return 0xFFFF;
    uint16_t Id = 0;
    for (int i = 4; i <= 6; i++) {
        const char c = Name[i];
        uint8_t d;
        if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
        else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
        else return 0xFFFF;
        Id = (uint16_t)((Id << 4) | d);
    }
    return Id;
}

inline uint32_t ScriptService::RdU32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

inline void ScriptService::MaskSet(uint16_t Slot, bool On) {
    uint16_t w = (uint16_t)(Slot >> 6), b = (uint16_t)(Slot & 63);
    if (On) ActiveMask[w] |= (uint64_t)1 << b;
    else    ActiveMask[w] &= ~((uint64_t)1 << b);
}

inline LoadedScript *ScriptService::Active(uint16_t Slot) {
    if (Slot >= MAX_SCRIPTS) return nullptr;
    return Registry[Slot].Active ? &Registry[Slot] : nullptr;
}

inline ValueInfo *ScriptService::AllocMetas(uint8_t Count) {
    if (Count == 0) return nullptr;
    return (ValueInfo *)calloc(Count, sizeof(ValueInfo));
}

// ===== Program compilation (instructions -> lines + If/While block table) =====

inline bool ScriptService::BuildProgram(LoadedScript *s, const uint8_t *Instr, uint32_t instrLen) {
    s->instrSymbols = (uint16_t)(instrLen / 4);
    if (s->instrSymbols) {
        s->Instr = (uint8_t *)malloc((size_t)s->instrSymbols * 4);
        if (!s->Instr) return false;
        memcpy(s->Instr, Instr, (size_t)s->instrSymbols * 4);
    }

    // First pass: count lines.
    uint16_t Count = 0;
    uint16_t i = 0;
    while (i < s->instrSymbols) {
        while (i < s->instrSymbols && s->Instr[i * 4] != SCRIPT_SYM_INSTRUCTION &&
               s->Instr[i * 4] != SCRIPT_SYM_ENDLINE) {
            i++;
        }
        if (i >= s->instrSymbols || s->Instr[i * 4] != SCRIPT_SYM_INSTRUCTION) {
            while (i < s->instrSymbols && s->Instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
            if (i < s->instrSymbols) i++;
            continue;
        }
        i++; // instruction
        while (i < s->instrSymbols && s->Instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
        if (i < s->instrSymbols) i++; // endline
        Count++;
        if (Count > SCRIPT_MAX_LINES) return false;
    }

    s->lineCount = Count;
    if (Count == 0) return true;
    s->Lines = (ScriptLineInfo *)calloc(Count, sizeof(ScriptLineInfo));
    s->blockMatch = (uint16_t *)malloc((size_t)Count * sizeof(uint16_t));
    s->blockKind = (uint8_t *)calloc(Count, 1);
    s->lineStamp = (uint32_t *)calloc(Count, sizeof(uint32_t));
    if (!s->Lines || !s->blockMatch || !s->blockKind || !s->lineStamp) return false;

    // Second pass: fill line descriptors and match If/While blocks. Mirrors the first
    // pass exactly (including orphan-symbol skipping) so indexes stay aligned.
    uint16_t *Stack = (uint16_t *)malloc((size_t)Count * sizeof(uint16_t));
    if (!Stack) return false;
    uint16_t Sp = 0;
    uint16_t Line = 0;
    i = 0;
    while (Line < Count) {
        uint16_t Start = i;
        while (i < s->instrSymbols && s->Instr[i * 4] != SCRIPT_SYM_INSTRUCTION &&
               s->Instr[i * 4] != SCRIPT_SYM_ENDLINE) {
            i++;
        }
        if (i >= s->instrSymbols || s->Instr[i * 4] != SCRIPT_SYM_INSTRUCTION) {
            while (i < s->instrSymbols && s->Instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
            if (i < s->instrSymbols) i++;
            continue;
        }
        uint8_t destCount = (uint8_t)(i - Start);
        uint8_t Cat = s->Instr[i * 4 + 1];
        uint8_t Op = s->Instr[i * 4 + 2];
        i++; // instruction
        uint16_t opStart = i;
        while (i < s->instrSymbols && s->Instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
        uint8_t opCount = (uint8_t)(i - opStart);
        if (i < s->instrSymbols) i++; // endline

        s->Lines[Line].Start = Start;
        s->Lines[Line].destCount = destCount;
        s->Lines[Line].opCount = opCount;
        s->blockMatch[Line] = 0xFFFF;

        if (Cat == SCRIPT_CAT_FLOW && (Op == SCRIPT_OP_FLOW_IF || Op == SCRIPT_OP_FLOW_WHILE)) {
            s->blockKind[Line] = (Op == SCRIPT_OP_FLOW_IF) ? 1 : 2;
            if (Sp < Count) Stack[Sp++] = Line;
        } else if (Cat == SCRIPT_CAT_FLOW && Op == SCRIPT_OP_FLOW_END) {
            s->blockKind[Line] = 3;
            if (Sp > 0) {
                uint16_t Open = Stack[--Sp];
                s->blockMatch[Open] = Line;
                s->blockMatch[Line] = Open;
            }
        }
        Line++;
    }
    free(Stack);
    return true;
}

// Parses SCR_<fileId> into registry `slot`. Docs/Services/Script.md: the loaded-script table
// maps a loaded id (the slot) to a file id, and the caller picks the slot - so the two need not
// be equal. The file id space is wider than the slot space (SCR_XXX vs the 6-bit instance), so
// the app stores many files and loads them into free slots. Returns false (and leaves the slot
// inactive) when the file is missing, malformed, or the slot/file id is out of range.
inline bool ScriptService::Load(uint16_t fileId, uint16_t Slot) {
    if (fileId >= MAX_SCRIPT_FILES || Slot >= MAX_SCRIPTS) return false;

    char Name[8];
    FileName(fileId, Name);
    uint32_t Size = Storage.FileExists(Name);
    if (Size == 0xFFFFFFFF || Size < SCRIPT_HEADER_SIZE) return false;

    uint8_t *Buf = (uint8_t *)malloc(Size);
    if (!Buf) return false;
    uint32_t Got = Storage.ReadFromFile(Name, 0, Size, (char *)Buf);
    if (Got < SCRIPT_HEADER_SIZE) { free(Buf); return false; }

    LoadedScript *s = &Registry[Slot];
    s->Release();
    MaskSet(Slot, false);
    s->Slot = Slot;
    s->fileId = fileId;
    s->Trid = (uint16_t)(TRID_SCRIPT_BASE + Slot);

    s->Properties = RdU32(Buf + 0);
    s->inCount    = Buf[4];
    s->outCount   = Buf[5];
    s->varCount   = Buf[6];
    s->constCount = Buf[7];
    uint32_t constLen = RdU32(Buf + 8);
    uint32_t defLen   = RdU32(Buf + 12);
    s->instrLen       = RdU32(Buf + 16);
    uint32_t uiLen    = RdU32(Buf + 20);

    uint32_t Entries = (uint32_t)(s->inCount + s->outCount + s->varCount + s->constCount);
    uint32_t Need = SCRIPT_HEADER_SIZE + Entries * 4 + constLen + defLen + s->instrLen + uiLen;
    if (Need > Got || Need > Size) { free(Buf); return false; }

    s->inMeta    = AllocMetas(s->inCount);
    s->outMeta   = AllocMetas(s->outCount);
    s->varMeta   = AllocMetas(s->varCount);
    s->constMeta = AllocMetas(s->constCount);
    if ((s->inCount && !s->inMeta) || (s->outCount && !s->outMeta) ||
        (s->varCount && !s->varMeta) || (s->constCount && !s->constMeta)) {
        s->Release(); free(Buf); return false;
    }

    uint32_t Off = SCRIPT_HEADER_SIZE;
    if (s->inCount)    { memcpy(s->inMeta,    Buf + Off, (size_t)s->inCount * 4);    Off += (uint32_t)s->inCount * 4; }
    if (s->outCount)   { memcpy(s->outMeta,   Buf + Off, (size_t)s->outCount * 4);   Off += (uint32_t)s->outCount * 4; }
    if (s->varCount)   { memcpy(s->varMeta,   Buf + Off, (size_t)s->varCount * 4);   Off += (uint32_t)s->varCount * 4; }
    if (s->constCount) { memcpy(s->constMeta, Buf + Off, (size_t)s->constCount * 4); Off += (uint32_t)s->constCount * 4; }

    // Prefix-sum the strides once: symbol resolution then costs a table lookup instead of a
    // walk over the preceding entries (the VM does that per operand per line per tick).
    if (!s->BuildOffsets()) { s->Release(); free(Buf); return false; }

    const uint8_t *constBlob = Buf + Off; Off += constLen;
    const uint8_t *defBlob   = Buf + Off; Off += defLen;
    const uint8_t *instrBlob = Buf + Off; Off += s->instrLen;
    const uint8_t *uiBlob    = Buf + Off; Off += uiLen;

    if (s->inTotal + s->outTotal) {
        s->ioSpace = (uint8_t *)calloc(1, (size_t)s->inTotal + s->outTotal);
        if (!s->ioSpace) { s->Release(); free(Buf); return false; }
    }
    s->varSpace = (uint8_t *)calloc(1, (size_t)(s->varTotal ? s->varTotal : 1));
    if (!s->varSpace) { s->Release(); free(Buf); return false; }
    if (s->constTotal) {
        s->constSpace = (uint8_t *)calloc(1, s->constTotal);
        if (!s->constSpace) { s->Release(); free(Buf); return false; }
    }

    // Compile the program (also copies the instruction symbols into RAM).
    if (!BuildProgram(s, instrBlob, s->instrLen)) { s->Release(); free(Buf); return false; }

    // Inputs carry their defaults from the file (same 4-byte-strided packing as the space,
    // so the file's offset is the space's own offset - no need to re-walk the strides).
    for (uint8_t i = 0; i < s->inCount; i++) {
        uint32_t Src = s->InputOffset(i);
        if (Src + s->inMeta[i].Size <= defLen)
            memcpy(s->ioSpace + Src, defBlob + Src, s->inMeta[i].Size);
    }
    for (uint8_t i = 0; i < s->constCount; i++) {
        uint32_t Src = s->ConstOffset(i);
        if (Src + s->constMeta[i].Size <= constLen)
            memcpy(s->constSpace + Src, constBlob + Src, s->constMeta[i].Size);
    }

    // UI info (version 1) starts with a length-prefixed function name; fall back to the
    // file name. The remaining names/specs are app-side editor metadata.
    s->uiLen = (uint16_t)uiLen;
    s->Name[0] = '\0';
    // UI info v2 starts with the version byte then the function name; the later sections
    // (enum option labels) are app-side only and ignored here. v1 is no longer supported.
    if (uiLen >= 2 && uiBlob[0] == 2) {
        uint8_t Nlen = uiBlob[1];
        if (2u + Nlen <= uiLen && Nlen < BLOCK_NAME_LEN) {
            memcpy(s->Name, uiBlob + 2, Nlen);
            s->Name[Nlen] = '\0';
        }
    }
    if (s->Name[0] == '\0') {
        memcpy(s->Name, Name, 6);
        s->Name[6] = '\0';
    }

    // Force the key byte to 0 and enforce read-only on outputs/constants.
    for (uint8_t i = 0; i < s->outCount; i++)   s->outMeta[i].Flags |= ValueReadOnly;
    for (uint8_t i = 0; i < s->constCount; i++) s->constMeta[i].Flags |= ValueReadOnly;

    s->Active = true;
    MaskSet(Slot, true);
    s->State = (uint8_t)ScriptState::Stopped;
    s->Ic = 0;
    free(Buf);
    return true;
}

inline void ScriptService::Unload(uint16_t Slot) {
    LoadedScript *s = Active(Slot);
    if (!s) return;
    s->Release();
    MaskSet(Slot, false);
}

// The loaded scripts' file ids (dense list, in slot order), each as two little-endian bytes.
inline uint16_t ScriptService::ListFiles(uint8_t *Out, uint16_t Max) {
    uint8_t n = 0;
    for (uint16_t i = 0; i < MAX_SCRIPTS && n < Max; i++)
        if (Registry[i].Active) {
            Out[2 * n] = (uint8_t)(Registry[i].fileId & 0xFF);
            Out[2 * n + 1] = (uint8_t)(Registry[i].fileId >> 8);
            n++;
        }
    return n;
}

inline uint8_t ScriptService::KeyCount(uint16_t Slot, uint8_t Field) {
    LoadedScript *s = Active(Slot);
    if (!s) return 0;
    switch (Field) {
        case SCRIPT_FIELD_INPUT:    return s->inCount;
        case SCRIPT_FIELD_OUTPUT:   return s->outCount;
        default: return 0; // Header/Variables/Constants are not register content
    }
}

// Non-copying I/O lookup for subscription sources and cross-service register access:
// returns a pointer straight into the I/O space (inputs then outputs).
inline bool ScriptService::GetIoPointer(uint16_t Slot, uint8_t Field, uint8_t Key, ValueInfo &m, void *&Data) {
    LoadedScript *s = Active(Slot);
    if (!s) return false;
    if (Field == SCRIPT_FIELD_INPUT && Key < s->inCount) {
        m = s->inMeta[Key];
        Data = s->ioSpace + s->InputOffset(Key);
        return true;
    }
    if (Field == SCRIPT_FIELD_OUTPUT && Key < s->outCount) {
        m = s->outMeta[Key];
        Data = s->ioSpace + s->OutputOffset(Key);
        return true;
    }
    return false;
}

// Resolves one I/O entry into a descriptor + value copy. Only inputs (field 1) and
// outputs (field 2) are exposed through the Register.
inline bool ScriptService::GetEntry(uint16_t Slot, uint8_t Field, uint8_t Key, ValueInfo &m, uint8_t *Vbuf, uint8_t &Vsz) {
    void *Data = nullptr;
    if (!GetIoPointer(Slot, Field, Key, m, Data)) return false;
    uint8_t n = m.Size;
    if (n) memcpy(Vbuf, Data, n);
    Vsz = n;
    return true;
}

// Writes an input (field 1) entry - the only writable Register entry of a script.
inline bool ScriptService::SetEntry(uint16_t Slot, uint8_t Field, uint8_t Key, const ValueInfo &m,
                           const uint8_t *Val, uint16_t Vlen) {
    LoadedScript *s = Active(Slot);
    if (!s || Field != SCRIPT_FIELD_INPUT || Key >= s->inCount) return false;
    const ValueInfo *Meta = &s->inMeta[Key];
    uint8_t *Data = s->ioSpace + s->InputOffset(Key);
    if (ValueIsReadOnly(*Meta)) return false;
    if (ValueInfoType(*Meta) != ValueInfoType(m)) return false;
    if (Vlen > Meta->Size) return false;
    if (Vlen) memcpy(Data, Val, Vlen);
    // A short write defines the rest of the fixed-size input too: spaces for a character
    // field (matching the static-block behaviour), zero otherwise, so no stale bytes survive.
    uint16_t Type = ValueInfoType(*Meta);
    uint8_t Fill = (Type == (uint16_t)DataType::Text || Type == (uint16_t)DataType::Name ||
                    Type == (uint16_t)DataType::Filename)
                       ? (uint8_t)' ' : 0;
    for (uint16_t i = Vlen; i < Meta->Size; i++) Data[i] = Fill;
    return true;
}

// Writes a variable's RAM (Script management CID 7 "Write Variable" - editor debug).
inline bool ScriptService::SetVariable(uint16_t Slot, uint8_t varId, const uint8_t *Val, uint16_t Vlen) {
    LoadedScript *s = Active(Slot);
    if (!s || varId >= s->varCount) return false;
    if (Vlen > s->varMeta[varId].Size) return false;
    if (Vlen) memcpy(s->varSpace + s->VarOffset(varId), Val, Vlen);
    return true;
}


#endif // USE_SCRIPTS
