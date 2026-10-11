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
        const char Char = Name[i];
        uint8_t Digit;
        if (Char >= '0' && Char <= '9') Digit = (uint8_t)(Char - '0');
        else if (Char >= 'A' && Char <= 'F') Digit = (uint8_t)(Char - 'A' + 10);
        else if (Char >= 'a' && Char <= 'f') Digit = (uint8_t)(Char - 'a' + 10);
        else return 0xFFFF;
        Id = (uint16_t)((Id << 4) | Digit);
    }
    return Id;
}

inline uint32_t ScriptService::RdU32(const uint8_t *Ptr) {
    uint32_t Raw; memcpy(&Raw, Ptr, 4); return Raw;
}

inline void ScriptService::MaskSet(uint16_t Slot, bool On) {
    uint16_t Word = (uint16_t)(Slot >> 6), Bit = (uint16_t)(Slot & 63);
    if (On) ActiveMask[Word] |= (uint64_t)1 << Bit;
    else    ActiveMask[Word] &= ~((uint64_t)1 << Bit);
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

inline bool ScriptService::BuildProgram(LoadedScript *Script, const uint8_t *Instr, uint32_t InstrLen) {
    Script->InstrSymbols = (uint16_t)(InstrLen / 4);
    if (Script->InstrSymbols) {
        Script->Instr = (uint8_t *)malloc((size_t)Script->InstrSymbols * 4);
        if (!Script->Instr) return false;
        memcpy(Script->Instr, Instr, (size_t)Script->InstrSymbols * 4);
    }

    // First pass: count lines.
    uint16_t Count = 0;
    uint16_t i = 0;
    while (i < Script->InstrSymbols) {
        while (i < Script->InstrSymbols && Script->Instr[i * 4] != SCRIPT_SYM_INSTRUCTION &&
               Script->Instr[i * 4] != SCRIPT_SYM_ENDLINE) {
            i++;
        }
        if (i >= Script->InstrSymbols || Script->Instr[i * 4] != SCRIPT_SYM_INSTRUCTION) {
            while (i < Script->InstrSymbols && Script->Instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
            if (i < Script->InstrSymbols) i++;
            continue;
        }
        i++; // instruction
        while (i < Script->InstrSymbols && Script->Instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
        if (i < Script->InstrSymbols) i++; // endline
        Count++;
        if (Count > SCRIPT_MAX_LINES) return false;
    }

    Script->LineCount = Count;
    if (Count == 0) return true;
    Script->Lines = (ScriptLineInfo *)calloc(Count, sizeof(ScriptLineInfo));
    Script->BlockMatch = (uint16_t *)malloc((size_t)Count * sizeof(uint16_t));
    Script->BlockKind = (uint8_t *)calloc(Count, 1);
    Script->LineStamp = (uint32_t *)calloc(Count, sizeof(uint32_t));
    if (!Script->Lines || !Script->BlockMatch || !Script->BlockKind || !Script->LineStamp) return false;

    // Second pass: fill line descriptors and match If/While blocks. Mirrors the first
    // pass exactly (including orphan-symbol skipping) so indexes stay aligned.
    uint16_t *Stack = (uint16_t *)malloc((size_t)Count * sizeof(uint16_t));
    if (!Stack) return false;
    uint16_t Sp = 0;
    uint16_t Line = 0;
    i = 0;
    while (Line < Count) {
        uint16_t Start = i;
        while (i < Script->InstrSymbols && Script->Instr[i * 4] != SCRIPT_SYM_INSTRUCTION &&
               Script->Instr[i * 4] != SCRIPT_SYM_ENDLINE) {
            i++;
        }
        if (i >= Script->InstrSymbols || Script->Instr[i * 4] != SCRIPT_SYM_INSTRUCTION) {
            while (i < Script->InstrSymbols && Script->Instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
            if (i < Script->InstrSymbols) i++;
            continue;
        }
        uint8_t DestCount = (uint8_t)(i - Start);
        uint8_t Cat = Script->Instr[i * 4 + 1];
        uint8_t Op = Script->Instr[i * 4 + 2];
        i++; // instruction
        uint16_t OpStart = i;
        while (i < Script->InstrSymbols && Script->Instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
        uint8_t OpCount = (uint8_t)(i - OpStart);
        if (i < Script->InstrSymbols) i++; // endline

        Script->Lines[Line].Start = Start;
        Script->Lines[Line].DestCount = DestCount;
        Script->Lines[Line].OpCount = OpCount;
        Script->BlockMatch[Line] = 0xFFFF;

        if (Cat == SCRIPT_CAT_FLOW && (Op == SCRIPT_OP_FLOW_IF || Op == SCRIPT_OP_FLOW_WHILE)) {
            Script->BlockKind[Line] = (Op == SCRIPT_OP_FLOW_IF) ? 1 : 2;
            if (Sp < Count) Stack[Sp++] = Line;
        } else if (Cat == SCRIPT_CAT_FLOW && Op == SCRIPT_OP_FLOW_END) {
            Script->BlockKind[Line] = 3;
            if (Sp > 0) {
                uint16_t Open = Stack[--Sp];
                Script->BlockMatch[Open] = Line;
                Script->BlockMatch[Line] = Open;
            }
        }
        Line++;
    }
    free(Stack);
    return true;
}

// Emits the load-time error log entry (Stage 1: a load failure logs through the Log service)
// and returns false, so every refusal in Load() reports exactly once. The code reuses the
// existing generic SCRIPT_ERR_* space: the load contract's coded fault is a memory-bound
// failure, so a refused load reports SCRIPT_ERR_BOUNDS rather than inventing a code.
inline bool ScriptService::LoadRefused(uint32_t Code) {
    ReportLog(MakeLog(false, (uint16_t)ServiceType::Script, Code, 0), PRIORITY_ERROR);
    return false;
}

// Parses SCR_<fileId> into registry `slot`. Docs/Services/Script.md: the loaded-script table
// maps a loaded id (the slot) to a file id, and the caller picks the slot - so the two need not
// be equal. The file id space is wider than the slot space (SCR_XXX vs the 6-bit instance), so
// the app stores many files and loads them into free slots. Returns false (and leaves the slot
// inactive) when the file is missing, malformed, or the slot/file id is out of range.
inline bool ScriptService::Load(uint16_t FileId, uint16_t Slot) {
    if (FileId >= MAX_SCRIPT_FILES || Slot >= MAX_SCRIPTS) return LoadRefused(SCRIPT_ERR_BOUNDS);

    char Name[8];
    FileName(FileId, Name);
    uint32_t Size = Storage.FileExists(Name);
    if (Size == 0xFFFFFFFF || Size < SCRIPT_HEADER_SIZE) return LoadRefused(SCRIPT_ERR_BOUNDS);

    uint8_t *Buf = (uint8_t *)malloc(Size);
    if (!Buf) return LoadRefused(SCRIPT_ERR_BOUNDS);
    uint32_t Got = Storage.ReadFromFile(Name, 0, Size, (char *)Buf);
    if (Got < SCRIPT_HEADER_SIZE) { free(Buf); return LoadRefused(SCRIPT_ERR_BOUNDS); }

    LoadedScript *Script = &Registry[Slot];
    Script->Release();
    MaskSet(Slot, false);
    Script->Slot = Slot;
    Script->FileId = FileId;
    Script->Trid = (uint16_t)(TRID_SCRIPT_BASE + Slot);

    ScriptHeader Hdr;
    if (!ParseScriptHeader(Buf, Got, Hdr)) { free(Buf); return LoadRefused(SCRIPT_ERR_BOUNDS); }
    Script->Properties = Hdr.Properties;
    Script->InCount    = Hdr.InCount;
    Script->OutCount   = Hdr.OutCount;
    Script->VarCount   = Hdr.VarCount;
    Script->ConstCount = Hdr.ConstCount;
    Script->FuncCount  = Hdr.FuncCount;
    uint32_t ConstLen = Hdr.ConstLen;
    uint32_t DefLen   = Hdr.DefLen;
    Script->InstrLen  = Hdr.InstrLen;
    uint32_t UiLen    = Hdr.UiLen;

    // The function reservations block (Function count x 16 bytes) sits between the input
    // defaults and the instructions. It is reserved here, not parsed: global linking is Stage 3.
    uint32_t FuncLen = (uint32_t)Script->FuncCount * SCRIPT_FUNC_RESERVATION_SIZE;

    uint32_t Entries = (uint32_t)(Script->InCount + Script->OutCount + Script->VarCount + Script->ConstCount);
    uint32_t Need = SCRIPT_HEADER_SIZE + Entries * 4 + ConstLen + DefLen + FuncLen + Script->InstrLen + UiLen;
    if (Need > Got || Need > Size) { free(Buf); return LoadRefused(SCRIPT_ERR_BOUNDS); }

    Script->InMeta    = AllocMetas(Script->InCount);
    Script->OutMeta   = AllocMetas(Script->OutCount);
    Script->VarMeta   = AllocMetas(Script->VarCount);
    Script->ConstMeta = AllocMetas(Script->ConstCount);
    if ((Script->InCount && !Script->InMeta) || (Script->OutCount && !Script->OutMeta) ||
        (Script->VarCount && !Script->VarMeta) || (Script->ConstCount && !Script->ConstMeta)) {
        Script->Release(); free(Buf); return LoadRefused(SCRIPT_ERR_BOUNDS);
    }

    uint32_t Off = SCRIPT_HEADER_SIZE;
    if (Script->InCount)    { memcpy(Script->InMeta,    Buf + Off, (size_t)Script->InCount * 4);    Off += (uint32_t)Script->InCount * 4; }
    if (Script->OutCount)   { memcpy(Script->OutMeta,   Buf + Off, (size_t)Script->OutCount * 4);   Off += (uint32_t)Script->OutCount * 4; }
    if (Script->VarCount)   { memcpy(Script->VarMeta,   Buf + Off, (size_t)Script->VarCount * 4);   Off += (uint32_t)Script->VarCount * 4; }
    if (Script->ConstCount) { memcpy(Script->ConstMeta, Buf + Off, (size_t)Script->ConstCount * 4); Off += (uint32_t)Script->ConstCount * 4; }

    // Prefix-sum the strides once: symbol resolution then costs a table lookup instead of a
    // walk over the preceding entries (the VM does that per operand per line per tick).
    if (!Script->BuildOffsets()) { Script->Release(); free(Buf); return LoadRefused(SCRIPT_ERR_BOUNDS); }

    const uint8_t *ConstBlob = Buf + Off; Off += ConstLen;
    const uint8_t *DefBlob   = Buf + Off; Off += DefLen;
    Off += FuncLen; // skip the function reservations block (linking is Stage 3)
    const uint8_t *InstrBlob = Buf + Off; Off += Script->InstrLen;
    const uint8_t *UiBlob    = Buf + Off; Off += UiLen;

    if (Script->InTotal + Script->OutTotal) {
        Script->IoSpace = (uint8_t *)calloc(1, (size_t)Script->InTotal + Script->OutTotal);
        if (!Script->IoSpace) { Script->Release(); free(Buf); return LoadRefused(SCRIPT_ERR_BOUNDS); }
    }
    Script->VarSpace = (uint8_t *)calloc(1, (size_t)(Script->VarTotal ? Script->VarTotal : 1));
    if (!Script->VarSpace) { Script->Release(); free(Buf); return LoadRefused(SCRIPT_ERR_BOUNDS); }
    if (Script->ConstTotal) {
        Script->ConstSpace = (uint8_t *)calloc(1, Script->ConstTotal);
        if (!Script->ConstSpace) { Script->Release(); free(Buf); return LoadRefused(SCRIPT_ERR_BOUNDS); }
    }

    // Compile the program (also copies the instruction symbols into RAM).
    if (!BuildProgram(Script, InstrBlob, Script->InstrLen)) { Script->Release(); free(Buf); return LoadRefused(SCRIPT_ERR_BOUNDS); }

    // Inputs carry their defaults from the file (same 4-byte-strided packing as the space,
    // so the file's offset is the space's own offset - no need to re-walk the strides).
    for (uint8_t i = 0; i < Script->InCount; i++) {
        uint32_t Src = Script->InputOffset(i);
        if (Src + Script->InMeta[i].Size <= DefLen)
            memcpy(Script->IoSpace + Src, DefBlob + Src, Script->InMeta[i].Size);
    }
    for (uint8_t i = 0; i < Script->ConstCount; i++) {
        uint32_t Src = Script->ConstOffset(i);
        if (Src + Script->ConstMeta[i].Size <= ConstLen)
            memcpy(Script->ConstSpace + Src, ConstBlob + Src, Script->ConstMeta[i].Size);
    }

    // UI info (version 1) starts with a length-prefixed function name; fall back to the
    // file name. The remaining names/specs are app-side editor metadata.
    Script->UiLen = (uint16_t)UiLen;
    Script->Name[0] = '\0';
    // UI info v2 starts with the version byte then the function name; the later sections
    // (enum option labels) are app-side only and ignored here. v1 is no longer supported.
    if (UiLen >= 2 && UiBlob[0] == 2) {
        uint8_t Nlen = UiBlob[1];
        if (2u + Nlen <= UiLen && Nlen < BLOCK_NAME_LEN) {
            memcpy(Script->Name, UiBlob + 2, Nlen);
            Script->Name[Nlen] = '\0';
        }
    }
    if (Script->Name[0] == '\0') {
        memcpy(Script->Name, Name, 6);
        Script->Name[6] = '\0';
    }

    // Force the key byte to 0 and enforce read-only on outputs/constants.
    for (uint8_t i = 0; i < Script->OutCount; i++)   Script->OutMeta[i].Flags |= ValueReadOnly;
    for (uint8_t i = 0; i < Script->ConstCount; i++) Script->ConstMeta[i].Flags |= ValueReadOnly;

    Script->Active = true;
    MaskSet(Slot, true);
    // Run-on-load applies whenever the script is loaded, not only during the boot scan
    // (Docs/Services/Script.md Properties: "Runs the script as soon as it is loaded").
    Script->State = ScriptPropRunOnLoad((uint16_t)Script->Properties)
                        ? (uint8_t)ScriptState::Running
                        : (uint8_t)ScriptState::Stopped;
    Script->Ic = 0;
    free(Buf);
    return true;
}

inline void ScriptService::Unload(uint16_t Slot) {
    LoadedScript *Script = Active(Slot);
    if (!Script) return;
    Script->Release();
    MaskSet(Slot, false);
}

// The loaded scripts' file ids (dense list, in slot order), each as two little-endian bytes.
inline uint16_t ScriptService::ListFiles(uint8_t *Out, uint16_t Max) {
    uint8_t Count = 0;
    for (uint16_t i = 0; i < MAX_SCRIPTS && Count < Max; i++)
        if (Registry[i].Active) {
            Out[2 * Count] = (uint8_t)(Registry[i].FileId & 0xFF);
            Out[2 * Count + 1] = (uint8_t)(Registry[i].FileId >> 8);
            Count++;
        }
    return Count;
}

inline uint8_t ScriptService::KeyCount(uint16_t Slot, uint8_t Field) {
    LoadedScript *Script = Active(Slot);
    if (!Script) return 0;
    switch (Field) {
        case SCRIPT_FIELD_INPUT:    return Script->InCount;
        case SCRIPT_FIELD_OUTPUT:   return Script->OutCount;
        default: return 0; // Header/Variables/Constants are not register content
    }
}

// Non-copying I/O lookup for subscription sources and cross-service register access:
// returns a pointer straight into the I/O space (inputs then outputs).
inline bool ScriptService::GetIoPointer(uint16_t Slot, uint8_t Field, uint8_t Key, ValueInfo &Info, void *&Data) {
    LoadedScript *Script = Active(Slot);
    if (!Script) return false;
    if (Field == SCRIPT_FIELD_INPUT && Key < Script->InCount) {
        Info = Script->InMeta[Key];
        Data = Script->IoSpace + Script->InputOffset(Key);
        return true;
    }
    if (Field == SCRIPT_FIELD_OUTPUT && Key < Script->OutCount) {
        Info = Script->OutMeta[Key];
        Data = Script->IoSpace + Script->OutputOffset(Key);
        return true;
    }
    return false;
}

// Resolves one I/O entry into a descriptor + value copy. Only inputs (field 1) and
// outputs (field 2) are exposed through the Register.
inline bool ScriptService::GetEntry(uint16_t Slot, uint8_t Field, uint8_t Key, ValueInfo &Info, uint8_t *Vbuf, uint8_t &Vsz) {
    void *Data = nullptr;
    if (!GetIoPointer(Slot, Field, Key, Info, Data)) return false;
    uint8_t Len = Info.Size;
    if (Len) memcpy(Vbuf, Data, Len);
    Vsz = Len;
    return true;
}

// Writes an input (field 1) entry - the only writable Register entry of a script.
inline bool ScriptService::SetEntry(uint16_t Slot, uint8_t Field, uint8_t Key, const ValueInfo &Info,
                           const uint8_t *Val, uint16_t Vlen) {
    LoadedScript *Script = Active(Slot);
    if (!Script || Field != SCRIPT_FIELD_INPUT || Key >= Script->InCount) return false;
    const ValueInfo *Meta = &Script->InMeta[Key];
    uint8_t *Data = Script->IoSpace + Script->InputOffset(Key);
    if (ValueIsReadOnly(*Meta)) return false;
    if (ValueInfoType(*Meta) != ValueInfoType(Info)) return false;
    if (Vlen > Meta->Size) return false;
    if (Vlen) memcpy(Data, Val, Vlen);
    // A short write defines the rest of the fixed-size input too. A fixed-size character
    // field is null-padded (Docs/Services/Register.md "a shorter value is null-padded at the
    // end"), and every other type is zero-filled as well, so no stale bytes survive. This
    // matches the static-block path (StaticBlockDescriptor::Set).
    memset(Data + Vlen, 0, (size_t)(Meta->Size - Vlen));
    return true;
}

// Writes a variable's RAM (Script management CID 7 "Write Variable" - editor debug).
inline bool ScriptService::SetVariable(uint16_t Slot, uint8_t VarId, const uint8_t *Val, uint16_t Vlen) {
    LoadedScript *Script = Active(Slot);
    if (!Script || VarId >= Script->VarCount) return false;
    if (Vlen > Script->VarMeta[VarId].Size) return false;
    if (Vlen) memcpy(Script->VarSpace + Script->VarOffset(VarId), Val, Vlen);
    return true;
}


#endif // USE_SCRIPTS
