#pragma once

// Tick, boot load and the management commands (ScriptRuntime.h) - part of the script service.
//
// The per-pass scheduler, the response handler, BootLoad
// and the 0x050X service commands.
//
// Split out of Script.h; included by it in order so the whole service stays one
// translation unit. Wrapped in the same guard so it is a no-op without USE_SCRIPTS.

#ifdef USE_SCRIPTS

#include "Core/Services/ScriptDefs.h"
#include "Core/Services/ScriptProgram.h"
#include "Core/Services/ScriptVm.h"
#include "Core/Services/ScriptExec.h"

inline void ScriptService::Run(LoadedScript *Script, uint32_t NowMs) {
    uint16_t Steps = 0;
    while (Steps++ < SCRIPT_INSTR_BUDGET) {
        if (Script->LineCount == 0 || Script->Ic >= Script->LineCount) { Script->State = (uint8_t)ScriptState::Finished; return; }
        if (Script->LineStamp && Script->LineStamp[Script->Ic] == TickCount) return; // looped this tick -> yield
        if (Script->LineStamp) Script->LineStamp[Script->Ic] = TickCount;
        bool Yield = false;
        uint8_t Err = ExecLine(Script, NowMs, Yield);
        if (Err) {
            // A run-time fault names the instruction behind it: the deepest frame's line (where
            // execution actually is) carries the offending Instruction symbol at start+destCount.
            // Its category/opcode encode through the single ScriptInstructionEncoding helper.
            const ScriptLineInfo &Ln = Script->Lines[Script->Ic];
            const uint8_t *Ins = Script->Instr + (size_t)(Ln.Start + Ln.DestCount) * 4;
            SetFault(Script, ScriptInstructionEncoding(Ins[1], Ins[2]), Err);
            return;
        }
        if (Yield) return;
        if (Script->State != (uint8_t)ScriptState::Running) return; // Finished/Error/Paused set by the op
    }
}

// Records a run-time fault into the CID 3 report bookkeeping and emits the run-time error log
// entry (Stage 1: a VM fault logs through the Log service). `Instruction` is the offending
// instruction's encoding from ScriptInstructionEncoding, or 0 for an engine-level fault - one
// that is not any instruction's doing. Setting the state clears the error, so SetState resets
// all of this bookkeeping along with the code.
inline void ScriptService::SetFault(LoadedScript *Script, uint32_t Instruction, uint32_t Err) {
    Script->ReportDepth = Script->CallDepth;
    Script->ReportLine = Script->Ic;
    Script->FaultInstruction = Instruction;
    Script->ErrorCode = Err;
    Script->State = (uint8_t)ScriptState::Error;
    ReportLog(MakeLog(false, (uint16_t)ServiceType::Script, Err, 0), PRIORITY_ERROR);
}

inline void ScriptService::Tick(uint32_t NowMs) {
    TickCount++;
    for (uint16_t Word = 0; Word < KScriptMaskWords; Word++) {
        uint64_t Mask = ActiveMask[Word];
        while (Mask) {
            uint16_t i = (uint16_t)(Word * 64 + __builtin_ctzll(Mask));
            Mask &= Mask - 1;
            LoadedScript *Script = &Registry[i];
            if (!Script->Active) continue;
            if (Script->State == (uint8_t)ScriptState::Running) {
                Run(Script, NowMs);
            } else if (Script->State == (uint8_t)ScriptState::Waiting) {
                if (Script->PendingForeign) {
                    if ((int32_t)(NowMs - Script->PendingDeadline) >= 0) {
                        Script->PendingForeign = false;
                        // Engine-level fault: a foreign confirmation never arrived, so no
                        // program instruction is behind it (instruction encoding 0).
                        SetFault(Script, 0, SCRIPT_ERR_TIMEOUT);
                    }
                    continue;
                }
                if (Script->WaitingOnTime && (int32_t)(NowMs - Script->WaitUntil) >= 0) {
                    Script->WaitingOnTime = false;
                    Script->State = (uint8_t)ScriptState::Running;
                } else if (!Script->WaitingOnTime) {
                    Script->State = (uint8_t)ScriptState::Running; // WaitUntil: re-evaluate
                }
            }
        }
    }
}

// Handles a reply to a foreign register request. The reply echoes the script's TRID, so the
// Dispatcher routes it here by range instead of by ServiceType.
inline void ScriptService::HandleResponse(const PacketFrame &Frame) {
    uint16_t Trid = Frame.Trid;
    for (uint16_t Word = 0; Word < KScriptMaskWords; Word++) {
    uint64_t Mask = ActiveMask[Word];
    while (Mask) {
        uint16_t i = (uint16_t)(Word * 64 + __builtin_ctzll(Mask));
        Mask &= Mask - 1;
        LoadedScript *Script = &Registry[i];
        if (!Script->Active || !Script->PendingForeign || Script->PendingTrid != Trid) continue;
        Script->PendingForeign = false;
        if (Script->PendingRead) {
            uint16_t Pb = PayloadBytes(Frame);
            if (Pb < 8) {
                // Engine-level: the reply is malformed, not any program instruction's doing.
                SetFault(Script, 0, SCRIPT_ERR_REGISTER);
                return;
            }
            ValueInfo Rm = *reinterpret_cast<const ValueInfo *>(Frame.Payload + 4);
            const uint8_t *Val = Frame.Payload + 8;
            uint8_t Avail = (uint8_t)(Pb - 8);
            uint16_t Vsz = Rm.Size;
            if (Vsz > Avail) Vsz = Avail;
            uint16_t Dtype = 0;
            uint8_t *Dest = nullptr;
            uint8_t Dsize = 0;
            if (ResolveDest(Script, Script->PendingDest, Dtype, Dest, Dsize))
                AssignResolved(Dtype, Dest, Dsize, Val, (uint8_t)Vsz, ValueInfoType(Rm.Type));
        }
        // Only a script still waiting on this confirmation resumes.
        if (Script->State == (uint8_t)ScriptState::Waiting)
            Script->State = (uint8_t)ScriptState::Running;
        return;
    }
    }
}

inline void ScriptService::SetState(LoadedScript *Script, uint8_t NewState) {
    if (NewState == (uint8_t)ScriptState::Running) {
        if (Script->State == (uint8_t)ScriptState::Stopped || Script->State == (uint8_t)ScriptState::Finished ||
            Script->State == (uint8_t)ScriptState::Error) {
            Script->Ic = 0;
            Script->CallDepth = 0;
        }
        Script->WaitingOnTime = false;
        Script->ErrorCode = SCRIPT_ERR_NONE;
        Script->FaultInstruction = 0;
        Script->ReportLine = 0;
        Script->ReportDepth = 0;
    } else if (NewState == (uint8_t)ScriptState::Stopped) {
        Script->Ic = 0;
        Script->CallDepth = 0;
        Script->WaitingOnTime = false;
        Script->ErrorCode = SCRIPT_ERR_NONE;
        Script->FaultInstruction = 0;
        Script->ReportLine = 0;
        Script->ReportDepth = 0;
    }
    // A manual state change abandons any outstanding foreign confirmation.
    if (NewState != (uint8_t)ScriptState::Waiting) Script->PendingForeign = false;
    Script->State = NewState;
}

// Boot: load every stored script flagged Load-on-boot; run those flagged Run-on-load.
// The lowest slot that holds no reservation (neither active nor load-failed), or MAX_SCRIPTS when
// all are in use. The per-word search is the host-testable ScriptLowestFreeBit helper. Bounded by
// MAX_SCRIPTS directly (not KScriptMaskWords, which is 0 when MAX_SCRIPTS is not a multiple of 64
// - e.g. the Valu's 16 slots).
inline uint16_t ScriptService::FreeSlot() {
    for (uint16_t Base = 0; Base < MAX_SCRIPTS; Base = (uint16_t)(Base + 64)) {
        uint64_t Occupied = 0;
        for (uint16_t b = 0; b < 64; b++) {
            uint16_t i = (uint16_t)(Base + b);
            if (i >= MAX_SCRIPTS) break;
            if (Registry[i].Active || Registry[i].LoadFailed) Occupied |= (uint64_t)1 << b;
        }
        uint16_t Bit = ScriptLowestFreeBit(Occupied);
        if (Bit < 64) {
            uint16_t i = (uint16_t)(Base + Bit);
            if (i < MAX_SCRIPTS) return i;
        }
        if ((uint16_t)(MAX_SCRIPTS - Base) <= 64) break;
    }
    return MAX_SCRIPTS;
}

// Loads every stored `SCR_XXX` that declares load-on-boot (Docs/Services/Script.md). A boot
// load is the flag bit plus the uint8 slot in Properties bits 8-15: deterministic, not the
// identity slot or the first free one, so the file-to-slot mapping is known without asking the
// device. The storage file table is scanned rather than a fixed id range, so a script file with
// a higher id is still pre-loaded. Run-on-load is honoured by Load() itself.
inline void ScriptService::BootLoad() {
    // `this` capture: the registry and the helpers are members now.
    Storage.ForEachFile([this](const char Name[8]) {
        uint16_t FileId = FileIdFromName(Name);
        if (FileId == 0xFFFF) return;
        // Peek the header's 16-bit Properties word so the declared slot is known before the
        // load allocates anything (a non-boot file is skipped without being loaded).
        uint8_t Hdr[2];
        if (Storage.ReadFromFile(Name, 0, 2, (char *)Hdr) < 2) return;
        uint16_t Props = (uint16_t)(Hdr[0] | (Hdr[1] << 8));
        if (!ScriptPropLoadOnBoot(Props)) return;
        uint16_t Slot = ScriptPropSlot(Props);
        if (Slot >= MAX_SCRIPTS) return;
        Load(FileId, Slot);
    });
}

// ===== Management commands (0x050X) =====

__attribute__((noinline)) inline bool ScriptService::Handle(const PacketFrame &Frame) {
    if (Frame.Flags & FLAG_TYPE) return true; // responses are not handled locally

    uint8_t Cid = GetServiceCID(Frame.Cmd);
    uint16_t Bytes = PayloadBytes(Frame);

    switch (Cid) {
        case 0: { // Get currently loaded scripts (their file ids, uint16 each)
            // 64 ids = 128 B > MAX_PAYLOAD_SIZE (116), so stream it as FRAG fragments exactly
            // like the Register enumerate; the app already reassembles those. The payload
            // length carries the count - no count byte (Docs/Services/Script.md CID 0).
            uint8_t Content[MAX_SCRIPTS * 2];
            uint16_t Count = ListFiles(Content, MAX_SCRIPTS);
            uint16_t Total = (uint16_t)(2 * Count);
            const uint16_t KFrag = 64;
            uint16_t Frags = (uint16_t)((Total + KFrag - 1) / KFrag);
            if (Frags == 0) Frags = 1;
            for (uint16_t Frag = 0; Frag < Frags; Frag++) {
                uint16_t Off = (uint16_t)(Frag * KFrag);
                uint16_t Len = (uint16_t)((Total - Off > KFrag) ? KFrag : Total - Off);
                memcpy(TxFrame.Payload + 4, Content + Off, Len);
                SendFragFragment(Frame, Frag, Frags, Len);
            }
            break;
        }

        case 1: { // Load Script (file id uint16 [, loaded id uint8]) -> Success
            // Docs/Services/Script.md CID 1: the caller may pick the loaded id (the slot), which
            // fails on an occupied slot; a load that names NO slot takes the lowest free one
            // (payload is then just the 2-byte file id). The reply is only Success.
            if (Bytes < 2) { RespondStatus(Frame, false); return true; }
            uint16_t FileId = (uint16_t)(Frame.Payload[0] | (Frame.Payload[1] << 8));
            uint16_t Slot;
            if (Bytes >= 3) {
                Slot = Frame.Payload[2];
            } else {
                Slot = FreeSlot();
                if (Slot >= MAX_SCRIPTS) { RespondStatus(Frame, false); return true; }
            }
            bool Ok = Load(FileId, Slot);
            RespondStatus(Frame, Ok);
            break;
        }

        case 2: { // Unload script (loaded ID)
            if (Bytes < 1) { RespondStatus(Frame, false); return true; }
            uint16_t Slot = Frame.Payload[0];
            bool Ok = Occupied(Slot); // active or load-failed both hold a reservation
            if (Ok) Unload(Slot);
            RespondStatus(Frame, Ok);
            break;
        }

        case 3: { // Read state (loaded ID -> report record; Docs/Services/Script.md "Report Record")
            if (Bytes < 1) { RespondStatus(Frame, false); return true; }
            LoadedScript *Script = Active(Frame.Payload[0]);
            if (!Script) { RespondStatus(Frame, false); return true; }
            // state (u8) | depth (u8) | line (u16): 4 bytes. On an error, instruction (u32) and
            // error (u32) are appended -> 12 bytes. `line` is the instruction counter of the
            // deepest frame (where execution actually is): the live Ic normally, the captured
            // fault line on an error. The depth byte pads `line` to a 2-byte boundary and the two
            // 32-bit error fields to 4-byte boundaries.
            const bool IsError = Script->State == (uint8_t)ScriptState::Error;
            const uint8_t Depth = IsError ? Script->ReportDepth : Script->CallDepth;
            const uint16_t Line = IsError ? Script->ReportLine : Script->Ic;
            uint8_t Reply[SCRIPT_REPORT_SIZE_ERR];
            Reply[SCRIPT_REPORT_OFF_STATE] = Script->State;
            Reply[SCRIPT_REPORT_OFF_DEPTH] = Depth;
            Reply[SCRIPT_REPORT_OFF_LINE] = (uint8_t)(Line & 0xFF);
            Reply[SCRIPT_REPORT_OFF_LINE + 1] = (uint8_t)(Line >> 8);
            uint16_t Len = SCRIPT_REPORT_SIZE;
            if (IsError) {
                const uint32_t Instruction = Script->FaultInstruction;
                const uint32_t Error = Script->ErrorCode;
                for (uint8_t k = 0; k < 4; k++) {
                    Reply[SCRIPT_REPORT_OFF_INSTRUCTION + k] = (uint8_t)(Instruction >> (8 * k));
                    Reply[SCRIPT_REPORT_OFF_ERROR + k] = (uint8_t)(Error >> (8 * k));
                }
                Len = SCRIPT_REPORT_SIZE_ERR;
            }
            SendResponse(Frame, Reply, Len);
            break;
        }

        case 4: { // Set state (loaded ID, action): the CID 4 action enum is the state verbs only
                  // - Start 9, Stop 10, Pause 11, Resume 12 (Docs/Services/Script.md CID 4). The
                  // Error op (13) is an in-script throw, not a management action, so it and any
                  // other value are rejected. Setting the state clears the error.
            if (Bytes < 2) { RespondStatus(Frame, false); return true; }
            LoadedScript *Script = Active(Frame.Payload[0]);
            if (!Script) { RespondStatus(Frame, false); return true; }
            uint8_t NewState;
            if (!ScriptActionToState(Frame.Payload[1], NewState)) { RespondStatus(Frame, false); return true; }
            SetState(Script, NewState);
            // docs: setting the state clears the error (and its report bookkeeping)
            Script->ErrorCode = SCRIPT_ERR_NONE;
            Script->FaultInstruction = 0;
            Script->ReportLine = 0;
            Script->ReportDepth = 0;
            RespondStatus(Frame, true);
            break;
        }

        case 5: { // Read internal state (loaded ID -> variable RAM + the instruction-counter
                  // call stack; Docs/Services/Script.md CID 5). The `line` moved to CID 3; this
                  // streams the rest of the internal picture, fragmented like CID 0.
            if (Bytes < 1) { RespondStatus(Frame, false); return true; }
            LoadedScript *Script = Active(Frame.Payload[0]);
            if (!Script) { RespondStatus(Frame, false); return true; }
            // Content: a leading depth byte, then the entire instruction-counter call stack -
            // the return-address stack Flow Call pushes and Flow Return pops, one u16 per frame
            // in push order (outermost first) - then the variable RAM (VarSpace, VarTotal
            // bytes). The depth byte delimits the variable-length stack so the reply decodes
            // from itself. Length can exceed one frame, so it streams as FRAG.
            const uint8_t Depth = Script->CallDepth;
            const uint16_t Vars = Script->VarTotal;
            const uint16_t StackBytes = (uint16_t)(Depth * 2);
            const uint16_t Total = (uint16_t)(1 + StackBytes + Vars);
            uint16_t Frags = (uint16_t)((Total + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
            if (Frags == 0) Frags = 1;
            for (uint16_t Frag = 0; Frag < Frags; Frag++) {
                uint16_t Off = (uint16_t)(Frag * MAX_FRAG_CONTENT_SIZE);
                uint16_t Len = (uint16_t)((Total - Off > MAX_FRAG_CONTENT_SIZE)
                                              ? MAX_FRAG_CONTENT_SIZE : (Total - Off));
                uint8_t *Dst = TxFrame.Payload + 4;
                for (uint16_t i = 0; i < Len; i++) {
                    uint16_t Src = (uint16_t)(Off + i);
                    if (Src == 0) {
                        Dst[i] = Depth;
                    } else if (Src < (uint16_t)(1 + StackBytes)) {
                        uint16_t Rel = (uint16_t)(Src - 1);    // byte offset into the stack
                        uint16_t Frame = (uint16_t)(Rel >> 1); // one u16 per frame
                        uint16_t Entry = Script->CallStack[Frame];
                        Dst[i] = (Rel & 1) ? (uint8_t)(Entry >> 8) : (uint8_t)(Entry & 0xFF);
                    } else {
                        Dst[i] = Script->VarSpace[Src - 1 - StackBytes];
                    }
                }
                SendFragFragment(Frame, Frag, Frags, Len);
            }
            break;
        }

        case 6: { // Move to instruction (loaded ID, line index u32)
            if (Bytes < 5) { RespondStatus(Frame, false); return true; }
            LoadedScript *Script = Active(Frame.Payload[0]);
            if (!Script) { RespondStatus(Frame, false); return true; }
            uint32_t Line = 0;
            memcpy(&Line, Frame.Payload + 1, 4);
            if (Script->LineCount && Line >= Script->LineCount) Line = Script->LineCount - 1;
            Script->Ic = (uint16_t)Line;
            RespondStatus(Frame, true);
            break;
        }

        case 7: { // Write Variable (loaded ID, variable ID, value)
            if (Bytes < 2) { RespondStatus(Frame, false); return true; }
            LoadedScript *Script = Active(Frame.Payload[0]);
            uint8_t VarId = Frame.Payload[1];
            if (!Script || VarId >= Script->VarCount) { RespondStatus(Frame, false); return true; }
            // The value length comes from the variable's declared size (the request carries
            // exactly that many value bytes).
            uint16_t Vlen = Script->VarMeta[VarId].Size;
            if ((uint16_t)(2 + Vlen) > Bytes) { RespondStatus(Frame, false); return true; }
            RespondStatus(Frame, SetVariable(Frame.Payload[0], VarId, Frame.Payload + 2, Vlen));
            break;
        }

        default:
            RespondStatus(Frame, false);
            break;
    }
    return true;
}


#endif // USE_SCRIPTS
