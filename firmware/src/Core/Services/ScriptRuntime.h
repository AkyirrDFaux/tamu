#pragma once

// Tick, boot load and the management commands (ScriptRuntime.h) - part of the script service.
//
// The per-pass scheduler, the not-found/response handlers, ScriptsBootLoad
// and the 0x050X service commands.
//
// Split out of Script.h; included by it in order so the whole service stays one
// translation unit. Wrapped in the same guard so it is a no-op without USE_SCRIPTS.

#ifdef USE_SCRIPTS

#include "Core/Services/ScriptDefs.h"
#include "Core/Services/ScriptProgram.h"
#include "Core/Services/ScriptVm.h"
#include "Core/Services/ScriptExec.h"

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
    for (uint16_t w = 0; w < kScriptMaskWords; w++) {
        uint64_t mask = scriptActiveMask[w];
        while (mask) {
            uint16_t i = (uint16_t)(w * 64 + __builtin_ctzll(mask));
            mask &= mask - 1;
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
}

// Handles a reply to a foreign register request. The reply echoes the script's TRID, so the
// Dispatcher routes it here by range instead of by ServiceType.
static void HandleScriptResponse(const PacketFrame &frame) {
    uint16_t trid = frame.trid;
    for (uint16_t w = 0; w < kScriptMaskWords; w++) {
    uint64_t mask = scriptActiveMask[w];
    while (mask) {
        uint16_t i = (uint16_t)(w * 64 + __builtin_ctzll(mask));
        mask &= mask - 1;
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
            ValueInfo rm = *reinterpret_cast<const ValueInfo *>(frame.payload + 4);
            const uint8_t *val = frame.payload + 8;
            uint8_t avail = (uint8_t)(pb - 8);
            uint16_t vsz = rm.Size;
            if (vsz > avail) vsz = avail;
            uint16_t dtype = 0;
            uint8_t *dest = nullptr;
            uint8_t dsize = 0;
            if (ScriptResolveDest(s, s->pendingDest, dtype, dest, dsize))
                ScriptAssignResolved(dtype, dest, dsize, val, (uint8_t)vsz, ValueInfoType(rm.Type));
        }
        // Only a script still waiting on this confirmation resumes.
        if (s->state == (uint8_t)ScriptState::Waiting)
            s->state = (uint8_t)ScriptState::Running;
        return;
    }
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
    // A manual state change abandons any outstanding foreign confirmation.
    if (newState != (uint8_t)ScriptState::Waiting) s->pendingForeign = false;
    s->state = newState;
}

// Boot: load every stored script flagged Load-on-boot; run those flagged Run-on-load.
// The first inactive loaded-script slot, or MAX_SCRIPTS when all are in use.
static uint16_t ScriptFreeSlot() {
    for (uint16_t i = 0; i < MAX_SCRIPTS; i++)
        if (!scriptRegistry[i].active) return i;
    return MAX_SCRIPTS;
}

// Loads every stored `SCR_XXX` flagged Load-on-boot into a slot
// (Docs/Services/Script.md). The storage file table is scanned rather than a fixed 0..63 id
// range, so a script file with a higher id is still pre-loaded. The identity slot (file N ->
// slot N) is preferred so the loaded table is deterministic across a reset; a file id beyond
// the slot space (or one whose identity slot is taken) uses the first free slot.
void ScriptsBootLoad() {
    Storage.ForEachFile([](const char name[8]) {
        uint16_t fileId = ScriptFileIdFromName(name);
        if (fileId == 0xFFFF) return;
        uint16_t slot = (fileId < MAX_SCRIPTS && !scriptRegistry[fileId].active)
                            ? fileId
                            : ScriptFreeSlot();
        if (slot >= MAX_SCRIPTS) return; // no free slot
        if (!ScriptLoad(fileId, slot)) return;
        if (!(scriptRegistry[slot].properties & SCRIPT_PROP_LOAD_ON_BOOT)) {
            scriptRegistry[slot].Release(); // stored, but not pre-loaded
            ScriptMaskSet(slot, false);
            return;
        }
        if (scriptRegistry[slot].properties & SCRIPT_PROP_RUN_ON_LOAD)
            scriptRegistry[slot].state = (uint8_t)ScriptState::Running;
    });
}

// ===== Management commands (0x050X) =====

__attribute__((noinline)) static void HandleScript(const PacketFrame &frame) {
    if (frame.flags & FLAG_TYPE) return; // responses are not handled locally

    uint8_t cid = GetServiceCID(frame.srv_tgt);
    uint16_t bytes = PayloadBytes(frame);

    switch (cid) {
        case 0: { // Get currently loaded scripts (their file ids, uint16 each)
            // 64 ids = 128 B > MAX_PAYLOAD_SIZE (116), so stream it as FRAG fragments exactly
            // like the Register enumerate; the app already reassembles those. The payload
            // length carries the count - no count byte (Docs/Services/Script.md CID 0).
            uint8_t content[MAX_SCRIPTS * 2];
            uint16_t n = ScriptListFiles(content, MAX_SCRIPTS);
            uint16_t total = (uint16_t)(2 * n);
            const uint16_t kFrag = 64;
            uint16_t frags = (uint16_t)((total + kFrag - 1) / kFrag);
            if (frags == 0) frags = 1;
            for (uint16_t f = 0; f < frags; f++) {
                uint16_t off = (uint16_t)(f * kFrag);
                uint16_t len = (uint16_t)((total - off > kFrag) ? kFrag : total - off);
                memcpy(tx_frame.payload + 4, content + off, len);
                SendFragFragment(frame, f, frags, len);
            }
            break;
        }

        case 1: { // Load Script (file id uint16, loaded id uint8) -> Success
            // Docs/Services/Script.md: the caller picks the loaded id (the slot); the reply is
            // just Success, so it must have chosen it.
            if (bytes < 3) { RespondStatus(frame, false); return; }
            uint16_t fileId = (uint16_t)(frame.payload[0] | (frame.payload[1] << 8));
            bool ok = ScriptLoad(fileId, frame.payload[2]);
            RespondStatus(frame, ok);
            break;
        }

        case 2: { // Unload script (loaded ID)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            uint16_t slot = frame.payload[0];
            bool ok = ScriptActive(slot) != nullptr;
            if (ok) ScriptUnload(slot);
            RespondStatus(frame, ok);
            break;
        }

        case 3: { // Read state (loaded ID -> state, last error code)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s) { RespondStatus(frame, false); return; }
            uint8_t reply[2] = { s->state, s->errorCode };
            SendResponse(frame, reply, 2);
            break;
        }

        case 4: { // Set state (loaded ID, new state; clears the error)
            if (bytes < 2) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s || frame.payload[1] > (uint8_t)ScriptState::Error) { RespondStatus(frame, false); return; }
            ScriptSetState(s, frame.payload[1]);
            s->errorCode = SCRIPT_ERR_NONE; // docs: setting the state clears the error
            RespondStatus(frame, true);
            break;
        }

        case 5: { // Read internal state (loaded ID -> instruction counter (line) + variable RAM)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s) { RespondStatus(frame, false); return; }
            // Streamed as FRAG: the 4-byte instruction counter then the variable RAM, which can
            // exceed one payload.
            uint32_t ic = s->ic;
            uint16_t total = (uint16_t)(4 + s->varTotal);
            uint16_t frags = (uint16_t)((total + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
            if (frags == 0) frags = 1;
            for (uint16_t f = 0; f < frags; f++) {
                uint16_t off = (uint16_t)(f * MAX_FRAG_CONTENT_SIZE);
                uint16_t len = (uint16_t)((total - off > MAX_FRAG_CONTENT_SIZE)
                                              ? MAX_FRAG_CONTENT_SIZE : (total - off));
                uint8_t *dst = tx_frame.payload + 4;
                for (uint16_t i = 0; i < len; i++) {
                    uint16_t src = (uint16_t)(off + i);
                    dst[i] = (src < 4) ? (uint8_t)(ic >> (8 * src)) : s->varSpace[src - 4];
                }
                SendFragFragment(frame, f, frags, len);
            }
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
            // The value length comes from the variable's declared size (the request carries
            // exactly that many value bytes).
            uint16_t vlen = s->varMeta[varId].Size;
            if ((uint16_t)(2 + vlen) > bytes) { RespondStatus(frame, false); return; }
            RespondStatus(frame, ScriptSetVariable(frame.payload[0], varId, frame.payload + 2, vlen));
            break;
        }

        default:
            RespondStatus(frame, false);
            break;
    }
}


#endif // USE_SCRIPTS
