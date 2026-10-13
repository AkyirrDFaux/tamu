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

inline void ScriptService::MaskSet(uint16_t Slot, bool On) {
    uint16_t Word = (uint16_t)(Slot >> 6), Bit = (uint16_t)(Slot & 63);
    if (On) ActiveMask[Word] |= (uint64_t)1 << Bit;
    else    ActiveMask[Word] &= ~((uint64_t)1 << Bit);
}

inline LoadedScript *ScriptService::Active(uint16_t Slot) {
    if (Slot >= MAX_SCRIPTS) return nullptr;
    return Registry[Slot].Active ? &Registry[Slot] : nullptr;
}

// A slot holds a reservation when it is active OR load-failed (the latter keeps only its slot
// record until an explicit unload clears it - Docs/Services/Script.md "State").
inline bool ScriptService::Occupied(uint16_t Slot) {
    if (Slot >= MAX_SCRIPTS) return false;
    return ScriptSlotReserved(Registry[Slot]);
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

    // Loading into an occupied slot fails: only empty slots accept a load (Docs/Services/Script.md
    // "Load Contract"). A load-failed slot also counts as occupied - its reservation is kept until
    // an explicit unload clears it - so a retry must unload first. The occupant is left untouched.
    if (Occupied(Slot)) return false;

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
    // defaults and the instructions.
    uint32_t Entries = (uint32_t)(Script->InCount + Script->OutCount + Script->VarCount + Script->ConstCount);
    uint32_t FuncLen = (uint32_t)Script->FuncCount * SCRIPT_FUNC_RESERVATION_SIZE;
    uint32_t Need = SCRIPT_HEADER_SIZE + Entries * 4 + ConstLen + DefLen + FuncLen + Script->InstrLen + UiLen;
    if (Need > Got || Need > Size) { free(Buf); return LoadRefused(SCRIPT_ERR_BOUNDS); }

    // Script-file blob offsets (the same block walk the header parser mirrors).
    uint32_t ConstOff = SCRIPT_HEADER_SIZE + Entries * 4;
    uint32_t DefOff   = ConstOff + ConstLen;
    uint32_t FuncOff  = DefOff + DefLen;
    uint32_t InstrOff = FuncOff + FuncLen;
    uint32_t UiOff    = InstrOff + Script->InstrLen;

    // ==== Metadata of the script itself ====
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

    const uint8_t *ConstBlob = Buf + ConstOff;
    const uint8_t *DefBlob   = Buf + DefOff;
    const uint8_t *FuncBlob  = Buf + FuncOff;
    const uint8_t *InstrBlob = Buf + InstrOff;
    const uint8_t *UiBlob    = Buf + UiOff;

    // ==== Stage 3: link the declared global functions (Docs/Services/Script.md "Functions and
    // Macros"). For each 16-byte reservation the device reads the function library file, places
    // the referenced body's memory and instruction space into this script's allocations and
    // patches the calls. A referenced function that is absent is a load error; it is detected
    // here, before anything is allocated, so a load-failed slot holds only its slot record.
    //
    // The reservation record's byte layout and the global-call operand encoding are both
    // UNDETERMINED by the spec; both are isolated in ScriptDefs.h (ParseFuncReservation,
    // ScriptGlobalCallOperand/ScriptIsGlobalCall). See the header comment there.
    struct Link {
        uint8_t **Buf = nullptr;         // one file image per reservation
        ScriptHeader *Hdr = nullptr;
        uint16_t *InstrOff = nullptr;    // reservation's instruction-space offset (symbols)
        uint16_t *SymBase = nullptr;     // this body's first symbol in the combined program
        uint16_t *SymCount = nullptr;    // symbols in this body
        uint16_t *LineBase = nullptr;    // this body's first line in the combined program
        uint16_t Count = 0;
        ~Link() {
            if (Buf) { for (uint16_t i = 0; i < Count; i++) free(Buf[i]); free(Buf); }
            free(Hdr); free(InstrOff); free(SymBase); free(SymCount); free(LineBase);
        }
    } Lk;

    uint8_t *Combined = nullptr;   // combined instruction blob (script program then bodies)
    uint32_t ProgLen = Script->InstrLen;
    const uint8_t *ProgBlob = InstrBlob;

    // Frees the transient allocations and refuses the load. `MarkFailed` records the load-failed
    // slot state (a link failure), leaving only the slot record behind.
    auto Refuse = [&](bool MarkFailed) -> bool {
        free(Combined);
        Script->Release();
        free(Buf);
        if (MarkFailed) { Script->State = (uint8_t)ScriptState::LoadFailed; Script->Slot = Slot; Script->FileId = FileId; }
        return LoadRefused(SCRIPT_ERR_BOUNDS);
    };

    Lk.Count = Script->FuncCount;
    if (Lk.Count) {
        Lk.Buf      = (uint8_t **)calloc(Lk.Count, sizeof(uint8_t *));
        Lk.Hdr      = (ScriptHeader *)calloc(Lk.Count, sizeof(ScriptHeader));
        Lk.InstrOff = (uint16_t *)calloc(Lk.Count, sizeof(uint16_t));
        Lk.SymBase  = (uint16_t *)calloc(Lk.Count, sizeof(uint16_t));
        Lk.SymCount = (uint16_t *)calloc(Lk.Count, sizeof(uint16_t));
        Lk.LineBase = (uint16_t *)calloc(Lk.Count, sizeof(uint16_t));
        if (!Lk.Buf || !Lk.Hdr || !Lk.InstrOff || !Lk.SymBase || !Lk.SymCount || !Lk.LineBase)
            return Refuse(false);

        uint32_t TInstrSym = (uint32_t)(Script->InstrLen / 4);
        uint32_t TVarMeta  = Script->VarCount;
        uint32_t TConstMeta = Script->ConstCount;

        for (uint16_t f = 0; f < Lk.Count; f++) {
            ScriptFuncReservation Res = ParseFuncReservation(FuncBlob + (uint32_t)f * SCRIPT_FUNC_RESERVATION_SIZE);
            if (Res.FileId >= MAX_SCRIPT_FILES) return Refuse(true);
            char FName[8];
            FileName(Res.FileId, FName);
            uint32_t FSize = Storage.FileExists(FName);
            // Link resolution: a referenced function that is absent is a load error.
            if (FSize == 0xFFFFFFFF || FSize < SCRIPT_HEADER_SIZE) return Refuse(true);
            uint8_t *FB = (uint8_t *)malloc(FSize);
            if (!FB) return Refuse(false);
            uint32_t FGot = Storage.ReadFromFile(FName, 0, FSize, (char *)FB);
            ScriptHeader FH;
            bool Ok = (FGot >= SCRIPT_HEADER_SIZE) && ParseScriptHeader(FB, FGot, FH)
                      && ScriptPropFunctionLib(FH.Properties);
            if (Ok) {
                uint32_t FEntries = (uint32_t)(FH.InCount + FH.OutCount + FH.VarCount + FH.ConstCount);
                uint32_t FNeed = SCRIPT_HEADER_SIZE + FEntries * 4 + FH.ConstLen + FH.DefLen
                               + (uint32_t)FH.FuncCount * SCRIPT_FUNC_RESERVATION_SIZE + FH.InstrLen + FH.UiLen;
                Ok = (FNeed <= FGot);
            }
            // A referenced file that is not a function library is as good as absent.
            if (!Ok) { free(FB); return Refuse(true); }
            Lk.Buf[f] = FB;
            Lk.Hdr[f] = FH;
            Lk.InstrOff[f] = Res.InstrOffset;

            uint32_t FInstrSym = (uint32_t)(FH.InstrLen / 4);
            uint32_t BodySym = (Res.InstrOffset < FInstrSym) ? (FInstrSym - Res.InstrOffset) : 0;
            Lk.SymBase[f] = (uint16_t)TInstrSym;
            Lk.SymCount[f] = (uint16_t)BodySym;
            TInstrSym  += BodySym;      // bodies append after the script and each other
            TVarMeta   += FH.VarCount;
            TConstMeta += FH.ConstCount;
        }

        // The merged counts are uint8 counts on every index path, so a link that would overflow
        // them is refused like any other memory-bound failure.
        if (TVarMeta > 255 || TConstMeta > 255) return Refuse(false);

        // Combined instruction blob: the script's program first (so its own line numbers are
        // unchanged), then each body, with the body's variable/constant symbol indices relocated
        // by the base it will occupy. A body's input/output symbols are NOT merged: the spec
        // makes a body's parameter channel its variables, so a well-formed body declares none -
        // a body that does is a documented limitation (see the TODO in the report).
        uint32_t TotalSym = TInstrSym;
        Combined = (uint8_t *)calloc(TotalSym ? TotalSym : 1, 4);
        if (!Combined) return Refuse(false);
        const uint32_t ScriptSym = (uint32_t)(Script->InstrLen / 4);
        if (ScriptSym) memcpy(Combined, InstrBlob, (size_t)ScriptSym * 4);
        const uint16_t VarBase = Script->VarCount, ConstBase = Script->ConstCount;
        for (uint16_t f = 0; f < Lk.Count; f++) {
            uint32_t FEntries = (uint32_t)(Lk.Hdr[f].InCount + Lk.Hdr[f].OutCount + Lk.Hdr[f].VarCount + Lk.Hdr[f].ConstCount);
            const uint8_t *FInstr = Lk.Buf[f] + SCRIPT_HEADER_SIZE + FEntries * 4
                                  + Lk.Hdr[f].ConstLen + Lk.Hdr[f].DefLen
                                  + (uint32_t)Lk.Hdr[f].FuncCount * SCRIPT_FUNC_RESERVATION_SIZE;
            const uint8_t *Src = FInstr + (size_t)Lk.InstrOff[f] * 4;
            uint8_t *Dst = Combined + (size_t)Lk.SymBase[f] * 4;
            for (uint16_t s = 0; s < Lk.SymCount[f]; s++) {
                uint16_t Val = ScriptRd16(Src + 2);
                if (Src[0] == SCRIPT_SYM_VARIABLE)      Val = (uint16_t)(Val + VarBase);
                else if (Src[0] == SCRIPT_SYM_CONSTANT) Val = (uint16_t)(Val + ConstBase);
                Dst[0] = Src[0]; Dst[1] = Src[1]; Dst[2] = (uint8_t)(Val & 0xFF); Dst[3] = (uint8_t)(Val >> 8);
                Src += 4; Dst += 4;
            }
        }
        ProgBlob = Combined;
        ProgLen  = TotalSym * 4;
        Script->InstrLen = ProgLen;

        // Merge the body variable/constant metadata after the script's own. In/Out are untouched.
        ValueInfo *NV = TVarMeta   ? (ValueInfo *)calloc(TVarMeta,   sizeof(ValueInfo)) : nullptr;
        ValueInfo *NC = TConstMeta ? (ValueInfo *)calloc(TConstMeta, sizeof(ValueInfo)) : nullptr;
        if ((TVarMeta && !NV) || (TConstMeta && !NC)) { free(NV); free(NC); return Refuse(false); }
        if (Script->VarCount)   memcpy(NV, Script->VarMeta,   (size_t)Script->VarCount * sizeof(ValueInfo));
        if (Script->ConstCount) memcpy(NC, Script->ConstMeta, (size_t)Script->ConstCount * sizeof(ValueInfo));
        free(Script->VarMeta); free(Script->ConstMeta);
        Script->VarMeta = NV; Script->ConstMeta = NC;
        uint16_t Vo = Script->VarCount, Co = Script->ConstCount;
        for (uint16_t f = 0; f < Lk.Count; f++) {
            const uint8_t *FM = Lk.Buf[f] + SCRIPT_HEADER_SIZE;
            if (Lk.Hdr[f].VarCount)
                memcpy(NV + Vo, FM + (uint32_t)(Lk.Hdr[f].InCount + Lk.Hdr[f].OutCount) * 4,
                       (size_t)Lk.Hdr[f].VarCount * sizeof(ValueInfo));
            Vo = (uint16_t)(Vo + Lk.Hdr[f].VarCount);
            if (Lk.Hdr[f].ConstCount)
                memcpy(NC + Co, FM + (uint32_t)(Lk.Hdr[f].InCount + Lk.Hdr[f].OutCount + Lk.Hdr[f].VarCount) * 4,
                       (size_t)Lk.Hdr[f].ConstCount * sizeof(ValueInfo));
            Co = (uint16_t)(Co + Lk.Hdr[f].ConstCount);
        }
        Script->VarCount   = (uint8_t)TVarMeta;
        Script->ConstCount = (uint8_t)TConstMeta;
    }

    // Prefix-sum the strides once: symbol resolution then costs a table lookup instead of a
    // walk over the preceding entries (the VM does that per operand per line per tick).
    if (!Script->BuildOffsets()) return Refuse(false);

    if (Script->InTotal + Script->OutTotal) {
        Script->IoSpace = (uint8_t *)calloc(1, (size_t)Script->InTotal + Script->OutTotal);
        if (!Script->IoSpace) return Refuse(false);
    }
    Script->VarSpace = (uint8_t *)calloc(1, (size_t)(Script->VarTotal ? Script->VarTotal : 1));
    if (!Script->VarSpace) return Refuse(false);
    if (Script->ConstTotal) {
        Script->ConstSpace = (uint8_t *)calloc(1, Script->ConstTotal);
        if (!Script->ConstSpace) return Refuse(false);
    }

    // Compile the program (also copies the instruction symbols into RAM).
    if (!BuildProgram(Script, ProgBlob, ProgLen)) return Refuse(false);
    free(Combined); Combined = nullptr;

    // Relocate the linked bodies' flow targets now that the line table exists: each body's lines
    // sit at LineBase[f], so a body-local Jump/Call target and each string of the script's own
    // global calls are patched (Docs: "only the address is filled in at load, by the same
    // relocation"). The operand is the app's `line` predefine (Predefine/Index); a line target
    // that is a variable/constant reference is not relocated (documented limitation).
    if (Lk.Count) {
        for (uint16_t f = 0; f < Lk.Count; f++) Lk.LineBase[f] = Script->LineCount; // sentinel
        for (uint16_t l = 0; l < Script->LineCount; l++) {
            uint16_t Start = Script->Lines[l].Start;
            for (uint16_t f = 0; f < Lk.Count; f++) {
                if (Lk.SymCount[f] && Start >= Lk.SymBase[f] && Start < (uint16_t)(Lk.SymBase[f] + Lk.SymCount[f])) {
                    if (Lk.LineBase[f] == Script->LineCount) Lk.LineBase[f] = l;
                    break;
                }
            }
        }
        for (uint16_t l = 0; l < Script->LineCount; l++) {
            const ScriptLineInfo &Ln = Script->Lines[l];
            const uint8_t *Ins = Script->Instr + (size_t)(Ln.Start + Ln.DestCount) * 4;
            if (Ins[1] != SCRIPT_CAT_FLOW || (Ins[2] != SCRIPT_OP_FLOW_JUMP && Ins[2] != SCRIPT_OP_FLOW_CALL)) continue;
            if (Ln.OpCount < 1) continue;
            uint8_t *Op = Script->Instr + (size_t)(Ln.Start + Ln.DestCount + 1) * 4;
            if (Op[0] != SCRIPT_SYM_PREDEFINE || Op[1] != SCRIPT_PRE_INDEX) continue;
            uint16_t Val = ScriptRd16(Op + 2);
            int Body = -1;
            for (uint16_t f = 0; f < Lk.Count; f++)
                if (Lk.SymCount[f] && Ln.Start >= Lk.SymBase[f] && Ln.Start < (uint16_t)(Lk.SymBase[f] + Lk.SymCount[f])) { Body = f; break; }
            uint16_t Target;
            if (Ins[2] == SCRIPT_OP_FLOW_CALL && Body < 0 && ScriptIsGlobalCall(Val)) {
                // A top-level global call: the operand is a reservation index -> that body's
                // first line. A nested global call (from inside a body) is not resolved here.
                uint16_t Idx = ScriptGlobalCallIndex(Val);
                if (Idx >= Lk.Count) continue; // unresolvable -> a run-time error when reached
                Target = Lk.LineBase[Idx];
                if (Target == Script->LineCount) continue;
            } else if (Body >= 0) {
                // A nested global call (a body linking another function) is not resolved here;
                // its operand keeps the global marker and fails at run time.
                if (ScriptIsGlobalCall(Val)) continue;
                if (Lk.LineBase[Body] == Script->LineCount) continue;
                Target = ScriptRelocate(Lk.LineBase[Body], Val); // body-local absolute line
            } else {
                continue; // a top-level local target is already absolute
            }
            Op[2] = (uint8_t)(Target & 0xFF);
            Op[3] = (uint8_t)(Target >> 8);
        }
    }

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
    // A linked body's constant values ride along with its metadata; its variables are the
    // parameter channel and carry no stored default, so they start zeroed.
    if (Lk.Count) {
        uint16_t FuncConsts = 0;
        for (uint16_t f = 0; f < Lk.Count; f++) FuncConsts = (uint16_t)(FuncConsts + Lk.Hdr[f].ConstCount);
        uint16_t Co = (uint16_t)(Script->ConstCount - FuncConsts); // first body's const meta base
        for (uint16_t f = 0; f < Lk.Count; f++) {
            uint32_t FEntries = (uint32_t)(Lk.Hdr[f].InCount + Lk.Hdr[f].OutCount + Lk.Hdr[f].VarCount + Lk.Hdr[f].ConstCount);
            const uint8_t *FConstBlob = Lk.Buf[f] + SCRIPT_HEADER_SIZE + FEntries * 4;
            uint32_t Base = Script->ConstOffset(Co);
            for (uint8_t i = 0; i < Lk.Hdr[f].ConstCount; i++) {
                uint32_t Own = (uint32_t)Script->ConstOffset((uint16_t)(Co + i)) - Base;
                uint8_t Sz = Script->ConstMeta[Co + i].Size;
                if (Lk.Hdr[f].ConstLen >= Own + Sz)
                    memcpy(Script->ConstSpace + Base + Own, FConstBlob + Own, Sz);
            }
            Co = (uint16_t)(Co + Lk.Hdr[f].ConstCount);
        }
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

// Clears a slot's reservation whether it is active or load-failed (Docs/Services/Script.md: a
// load-failed slot "keeps its reservation until an explicit unload clears it").
inline void ScriptService::Unload(uint16_t Slot) {
    if (Slot >= MAX_SCRIPTS) return;
    Registry[Slot].Release();
    MaskSet(Slot, false);
}

// The whole slot map, in slot order, one uint16 file id per slot (0xFFFF for an empty slot);
// each id is two little-endian bytes. A load-failed slot still holds its reservation, so it
// appears with its file id too. Returns the slot count written.
inline uint16_t ScriptService::ListFiles(uint8_t *Out, uint16_t Max) {
    // The WHOLE slot map, in slot order: one uint16 file id per slot, 0xFFFF for an empty
    // (unreserved) slot, so the stream length gives the device's slot count and the app need
    // not assume 256 (Docs/Services/Script.md CID 0).
    uint16_t Slots = (MAX_SCRIPTS < Max) ? MAX_SCRIPTS : Max;
    for (uint16_t i = 0; i < Slots; i++) {
        uint16_t Id = ScriptSlotReserved(Registry[i]) ? Registry[i].FileId : 0xFFFF;
        Out[2 * i] = (uint8_t)(Id & 0xFF);
        Out[2 * i + 1] = (uint8_t)(Id >> 8);
    }
    return Slots;
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
