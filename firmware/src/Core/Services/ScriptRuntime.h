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
        if (Err) { Script->ErrorCode = Err; Script->State = (uint8_t)ScriptState::Error; return; }
        if (Yield) return;
        if (Script->State != (uint8_t)ScriptState::Running) return; // Finished/Error/Paused set by the op
    }
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
                        Script->ErrorCode = SCRIPT_ERR_TIMEOUT;
                        Script->State = (uint8_t)ScriptState::Error;
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
                Script->ErrorCode = SCRIPT_ERR_REGISTER;
                Script->State = (uint8_t)ScriptState::Error;
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
    } else if (NewState == (uint8_t)ScriptState::Stopped) {
        Script->Ic = 0;
        Script->CallDepth = 0;
        Script->WaitingOnTime = false;
        Script->ErrorCode = SCRIPT_ERR_NONE;
    }
    // A manual state change abandons any outstanding foreign confirmation.
    if (NewState != (uint8_t)ScriptState::Waiting) Script->PendingForeign = false;
    Script->State = NewState;
}

// Boot: load every stored script flagged Load-on-boot; run those flagged Run-on-load.
// The first inactive loaded-script slot, or MAX_SCRIPTS when all are in use.
inline uint16_t ScriptService::FreeSlot() {
    for (uint16_t i = 0; i < MAX_SCRIPTS; i++)
        if (!Registry[i].Active) return i;
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

        case 1: { // Load Script (file id uint16, loaded id uint8) -> Success
            // Docs/Services/Script.md: the caller picks the loaded id (the slot); the reply is
            // just Success, so it must have chosen it.
            if (Bytes < 3) { RespondStatus(Frame, false); return true; }
            uint16_t FileId = (uint16_t)(Frame.Payload[0] | (Frame.Payload[1] << 8));
            bool Ok = Load(FileId, Frame.Payload[2]);
            RespondStatus(Frame, Ok);
            break;
        }

        case 2: { // Unload script (loaded ID)
            if (Bytes < 1) { RespondStatus(Frame, false); return true; }
            uint16_t Slot = Frame.Payload[0];
            bool Ok = Active(Slot) != nullptr;
            if (Ok) Unload(Slot);
            RespondStatus(Frame, Ok);
            break;
        }

        case 3: { // Read state (loaded ID -> state, last error code)
            if (Bytes < 1) { RespondStatus(Frame, false); return true; }
            LoadedScript *Script = Active(Frame.Payload[0]);
            if (!Script) { RespondStatus(Frame, false); return true; }
            uint8_t Reply[2] = { Script->State, Script->ErrorCode };
            SendResponse(Frame, Reply, 2);
            break;
        }

        case 4: { // Set state (loaded ID, action): the CID 4 action enum, mirroring the Service
                  // state-op instructions - Start 9, Stop 10, Pause 11, Resume 12, Error 13
                  // (Docs/Services/Script.md CID 4). Setting the state clears the error.
            if (Bytes < 2) { RespondStatus(Frame, false); return true; }
            LoadedScript *Script = Active(Frame.Payload[0]);
            if (!Script) { RespondStatus(Frame, false); return true; }
            uint8_t NewState;
            switch (Frame.Payload[1]) {
                case SCRIPT_OP_SERVICE_START:  NewState = (uint8_t)ScriptState::Running; break;
                case SCRIPT_OP_SERVICE_STOP:   NewState = (uint8_t)ScriptState::Stopped; break;
                case SCRIPT_OP_SERVICE_PAUSE:  NewState = (uint8_t)ScriptState::Paused;  break;
                case SCRIPT_OP_SERVICE_RESUME: NewState = (uint8_t)ScriptState::Running; break;
                case SCRIPT_OP_SERVICE_ERROR:  NewState = (uint8_t)ScriptState::Error;   break;
                default: RespondStatus(Frame, false); return true;
            }
            SetState(Script, NewState);
            Script->ErrorCode = SCRIPT_ERR_NONE; // docs: setting the state clears the error
            RespondStatus(Frame, true);
            break;
        }

        case 5: { // Read internal state (loaded ID -> instruction counter (line) + variable RAM)
            if (Bytes < 1) { RespondStatus(Frame, false); return true; }
            LoadedScript *Script = Active(Frame.Payload[0]);
            if (!Script) { RespondStatus(Frame, false); return true; }
            // Streamed as FRAG: the 4-byte instruction counter then the variable RAM, which can
            // exceed one payload.
            uint32_t Ic = Script->Ic;
            uint16_t Total = (uint16_t)(4 + Script->VarTotal);
            uint16_t Frags = (uint16_t)((Total + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
            if (Frags == 0) Frags = 1;
            for (uint16_t Frag = 0; Frag < Frags; Frag++) {
                uint16_t Off = (uint16_t)(Frag * MAX_FRAG_CONTENT_SIZE);
                uint16_t Len = (uint16_t)((Total - Off > MAX_FRAG_CONTENT_SIZE)
                                              ? MAX_FRAG_CONTENT_SIZE : (Total - Off));
                uint8_t *Dst = TxFrame.Payload + 4;
                for (uint16_t i = 0; i < Len; i++) {
                    uint16_t Src = (uint16_t)(Off + i);
                    Dst[i] = (Src < 4) ? (uint8_t)(Ic >> (8 * Src)) : Script->VarSpace[Src - 4];
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
