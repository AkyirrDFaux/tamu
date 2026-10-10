#pragma once

// Instruction execution (one line at a time) (ScriptExec.h) - part of the script service.
//
// Set/Limit/Min/Max/Transform, flow, time, service and compose instructions.
//
// Split out of Script.h; included by it in order so the whole service stays one
// translation unit. Wrapped in the same guard so it is a no-op without USE_SCRIPTS.

#ifdef USE_SCRIPTS

#include "Core/Services/ScriptDefs.h"
#include "Core/Services/ScriptVm.h"
#include "Core/Services/ScriptExpr.h"
#include "Core/Types/Number.h"


// Evaluates the Set line's token stream and stores it into the destination.
inline uint8_t ScriptService::ExecSet(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                             uint16_t Dtype, uint8_t *Dest, uint8_t Dsize)
{
    if (Ln.opCount < 1) return SCRIPT_ERR_OPERAND;
    ExprParser p;
    p.s = s;
    p.Tok = Opbase;
    p.n = Ln.opCount;
    p.i = 0;
    p.Depth = 0;
    ExprValue v;
    if (!ExprBin(&p, v, 1)) return SCRIPT_ERR_OPERAND;
    if (p.i != p.n) return SCRIPT_ERR_OPERAND; // trailing tokens

    if (Dtype == (uint16_t)DataType::Vector)
    {
        uint8_t Dcount = Dsize / 4;
        if (Dcount == 0 || v.Count != Dcount) return SCRIPT_ERR_TYPE;
        for (uint8_t k = 0; k < Dcount; k++)
        {
            int32_t Raw = v.e[k].Value;
            memcpy(Dest + (size_t)k * 4, &Raw, 4);
        }
    }
    else if (Dtype == (uint16_t)DataType::Matrix)
    {
        // A Matrix destination takes its dimensions from the value's header (the dest slot
        // may be uninitialised). The value must be a Matrix (or a Matrix expression result).
        if (v.Dtype != (uint16_t)DataType::Matrix || v.Count == 0) return SCRIPT_ERR_TYPE;
        if ((uint32_t)Dsize < 4u + (uint32_t)v.Count * 4u) return SCRIPT_ERR_TYPE;
        Dest[0] = (uint8_t)(v.Mh & 0xFF);
        Dest[1] = (uint8_t)(v.Mh >> 8);
        Dest[2] = (uint8_t)(v.Mw & 0xFF);
        Dest[3] = (uint8_t)(v.Mw >> 8);
        for (uint8_t k = 0; k < v.Count; k++)
        {
            int32_t Raw = v.e[k].Value;
            memcpy(Dest + 4 + (size_t)k * 4, &Raw, 4);
        }
    }
    else
    {
        if (v.Count != 1) return SCRIPT_ERR_TYPE;
        ScriptScalar Sc;
        Sc.n = v.e[0];
        Sc.i = v.e[0].RoundToInt();
        StoreScalar(Dtype, Dest, Dsize, Dtype == (uint16_t)DataType::Number, Sc);
    }
    s->Ic++;
    return SCRIPT_ERR_NONE;
}

// Evaluates a token stream as an expression and returns its truth value (used by the flow
// instructions and Wait until).
inline bool ScriptService::ExprTruthy(LoadedScript *s, const uint8_t *Tok, uint8_t n, bool &Ok)
{
    if (n < 1) return false;
    ExprParser p;
    p.s = s;
    p.Tok = Tok;
    p.n = n;
    p.i = 0;
    p.Depth = 0;
    ExprValue v;
    if (!ExprBin(&p, v, 1)) return false;
    if (p.i != p.n) return false;
    Ok = (v.Count > 0) && (v.e[0].Value != 0);
    return true;
}

// Transform dest = rot, ox, oy, sx, sy [, skew] -> a 2x3 affine matrix (rotation in
// radians), matching the render Position/Offset format.
inline uint8_t ScriptService::ExecTransform(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                                   uint16_t Dtype, uint8_t *Dest, uint8_t Dsize)
{
    if (Dtype != (uint16_t)DataType::Matrix || Dsize < 4 + 6 * 4) return SCRIPT_ERR_TYPE;
    if (Ln.opCount < 5) return SCRIPT_ERR_OPERAND;
    Number v[6] = {Number(0), Number(0), Number(0), Number(0), Number(0), Number(0)};
    uint8_t n = Ln.opCount;
    if (n > 6) n = 6;
    for (uint8_t i = 0; i < n; i++)
    {
        ScriptScalar Sc;
        if (!ResolveOperandScalar(s, Opbase + (size_t)i * 4, true, Sc)) return SCRIPT_ERR_OPERAND;
        v[i] = Sc.n;
    }
    Number Rot = v[0], Tx = v[1], Ty = v[2], Sx = v[3], Sy = v[4], Skew = v[5];
    Number c = cos(Rot), S2 = sin(Rot);
    Number k = Number(0);
    if (Skew.Value != 0)
    {
        Number Cd = cos(Skew);
        if (Cd.Value != 0) k = sin(Skew) / Cd;
    }
    // Cells [a b tx; c d ty] (matches the app's Transform23).
    Number Cells[6];
    Cells[0] = Sx * c;
    Cells[1] = Sx * c * k + Sy * S2;
    Cells[2] = Tx;
    Cells[3] = -Sx * S2;
    Cells[4] = -Sx * S2 * k + Sy * c;
    Cells[5] = Ty;
    // The renderer samples the geometry mask forward (pp = Position * coord), so the shape's
    // centre lands where the sampled point is zero: Position^-1(0) = -L^-1 * t. To keep the
    // centre where the caller asked for it (-t, the same as an unrotated translation) the
    // stored translation is pre-rotated by the linear part: t' = L * t.
    Number Txr = Cells[0] * Tx + Cells[1] * Ty;
    Number Tyr = Cells[3] * Tx + Cells[4] * Ty;
    Cells[2] = Txr;
    Cells[5] = Tyr;
    Dest[0] = 2; Dest[1] = 0; Dest[2] = 3; Dest[3] = 0;
    for (uint8_t i = 0; i < 6; i++)
    {
        int32_t Raw = Cells[i].Value;
        memcpy(Dest + 4 + (size_t)i * 4, &Raw, 4);
    }
    s->Ic++;
    return SCRIPT_ERR_NONE;
}

inline uint8_t ScriptService::ExecMath(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Cat, uint8_t Op) {
    if (Ln.destCount < 1) return SCRIPT_ERR_OPERAND;
    uint16_t Dtype = 0;
    uint8_t *Dest = nullptr;
    uint8_t Dsize = 0;
    if (!ResolveDest(s, s->Instr + (size_t)Ln.Start * 4, Dtype, Dest, Dsize)) return SCRIPT_ERR_OPERAND;
    bool Num = (Dtype == (uint16_t)DataType::Number);

    // SET evaluates an infix expression (scalar / vector / matrix).
    if (Cat == SCRIPT_CAT_MATH && Op == SCRIPT_OP_MATH_SET) {
        return ExecSet(s, Ln, Opbase, Dtype, Dest, Dsize);
    }

    // Transform: a 2x3 affine matrix from (rot, ox, oy, sx, sy[, skew]).
    if (Cat == SCRIPT_CAT_MATH && Op == SCRIPT_OP_MATH_TRANSFORM) {
        return ExecTransform(s, Ln, Opbase, Dtype, Dest, Dsize);
    }

    // Vector/Matrix destination: element-wise math (operands resolved raw).
    if (Dtype == (uint16_t)DataType::Vector || Dtype == (uint16_t)DataType::Matrix) {
        return ExecVectorMath(s, Ln, Opbase, Cat, Op, Dtype, Dest, Dsize);
    }

    ScriptScalar In[8];
    uint8_t n = Ln.opCount;
    if (n > 8) n = 8;
    if (n < 1) return SCRIPT_ERR_OPERAND;
    for (uint8_t i = 0; i < n; i++) {
        if (!ResolveOperandScalar(s, Opbase + (size_t)i * 4, Num, In[i])) return SCRIPT_ERR_OPERAND;
    }

    ScriptScalar Out;
    Out.n = Number(0);
    Out.i = 0;
    switch (Cat) {
        case SCRIPT_CAT_MATH:
            // Fold every operand left-to-right (n-ary: dest = a op b op c ...).
            Out = In[0];
            switch (Op) {
                case SCRIPT_OP_MATH_MOD:
                    for (uint8_t k = 1; k < n; k++) {
                        if (Num) { int32_t b = In[k].n.RoundToInt(); if (b == 0) return SCRIPT_ERR_TYPE; Out.n = Number(Out.n.RoundToInt() % b); }
                        else { if (In[k].i == 0) return SCRIPT_ERR_TYPE; Out.i %= In[k].i; }
                    }
                    break;
                case SCRIPT_OP_MATH_MIN:
                    for (uint8_t k = 1; k < n; k++) { if (Num) Out.n = min(Out.n, In[k].n); else Out.i = Out.i < In[k].i ? Out.i : In[k].i; }
                    break;
                case SCRIPT_OP_MATH_MAX:
                    for (uint8_t k = 1; k < n; k++) { if (Num) Out.n = max(Out.n, In[k].n); else Out.i = Out.i > In[k].i ? Out.i : In[k].i; }
                    break;
                case SCRIPT_OP_MATH_ABS: if (Num) Out.n = abs(Out.n); else Out.i = Out.i < 0 ? -Out.i : Out.i; break;
                case SCRIPT_OP_MATH_LIMIT: {
                    if (n < 3) return SCRIPT_ERR_OPERAND;
                    if (Num) {
                        Number Lo = In[1].n, Hi = In[2].n;
                        if (Out.n.Value < Lo.Value) Out.n = Lo;
                        if (Out.n.Value > Hi.Value) Out.n = Hi;
                    } else {
                        int32_t Lo = In[1].i, Hi = In[2].i;
                        if (Out.i < Lo) Out.i = Lo;
                        if (Out.i > Hi) Out.i = Hi;
                    }
                    break;
                }
                default: return SCRIPT_ERR_UNKNOWN_OP;
            }
            break;
        case SCRIPT_CAT_LOGIC:
            // Comparisons/logic moved into the expression (Set/If/While); only Select remains.
            switch (Op) {
                case SCRIPT_OP_LOGIC_SELECT: {
                    if (n < 3) return SCRIPT_ERR_OPERAND;
                    bool Cond = Num ? (In[0].n.Value != 0) : (In[0].i != 0);
                    if (Num) Out.n = Cond ? In[1].n : In[2].n; else Out.i = Cond ? In[1].i : In[2].i;
                    break;
                }
                default: return SCRIPT_ERR_UNKNOWN_OP;
            }
            break;
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }

    StoreScalar(Dtype, Dest, Dsize, Num, Out);
    s->Ic++;
    return SCRIPT_ERR_NONE;
}

inline uint8_t ScriptService::ExecFlow(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op) {
    switch (Op) {
        case SCRIPT_OP_FLOW_IF: {
            if (Ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool Ok = false;
            if (!ExprTruthy(s, Opbase, Ln.opCount, Ok)) return SCRIPT_ERR_OPERAND;
            uint16_t Match = s->blockMatch[s->Ic];
            s->Ic = Ok ? s->Ic + 1 : (Match == 0xFFFF ? s->Ic + 1 : Match + 1);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_WHILE: {
            if (Ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool Ok = false;
            if (!ExprTruthy(s, Opbase, Ln.opCount, Ok)) return SCRIPT_ERR_OPERAND;
            uint16_t Match = s->blockMatch[s->Ic];
            s->Ic = Ok ? s->Ic + 1 : (Match == 0xFFFF ? s->Ic + 1 : Match + 1);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_END: {
            uint16_t Open = (s->blockMatch[s->Ic] == 0xFFFF) ? 0xFFFF : s->blockMatch[s->Ic];
            if (Open != 0xFFFF && s->blockKind[Open] == 2) { s->Ic = Open; return SCRIPT_ERR_NONE; } // while -> re-check
            s->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_JUMP: {
            if (Ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t Target = 0;
            if (!ResolveOperandInt(s, Opbase, Target)) return SCRIPT_ERR_OPERAND;
            if (Target < 0 || Target >= s->lineCount) return SCRIPT_ERR_BOUNDS;
            s->Ic = (uint16_t)Target;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_CALL: {
            if (Ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t Target = 0;
            if (!ResolveOperandInt(s, Opbase, Target)) return SCRIPT_ERR_OPERAND;
            if (Target < 0 || Target >= s->lineCount) return SCRIPT_ERR_BOUNDS;
            if (s->callDepth >= SCRIPT_MAX_CALL_DEPTH) return SCRIPT_ERR_STACK;
            s->callStack[s->callDepth++] = (uint16_t)(s->Ic + 1);
            s->Ic = (uint16_t)Target;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_RETURN: {
            if (s->callDepth == 0) { s->State = (uint8_t)ScriptState::Finished; return SCRIPT_ERR_NONE; }
            s->Ic = s->callStack[--s->callDepth];
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_HALT:
            s->State = (uint8_t)ScriptState::Finished;
            return SCRIPT_ERR_NONE;
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}

inline uint8_t ScriptService::ExecTime(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op, uint32_t nowMs, bool &Yield) {
    switch (Op) {
        case SCRIPT_OP_TIME_DELAY: {
            if (Ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t Ms = 0;
            if (!ResolveOperandInt(s, Opbase, Ms)) return SCRIPT_ERR_OPERAND;
            if (Ms < 0) Ms = 0;
            s->Ic++;
            s->waitUntil = nowMs + (uint32_t)Ms;
            s->waitingOnTime = true;
            s->State = (uint8_t)ScriptState::Waiting;
            Yield = true;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_TIME_WAIT: {
            if (Ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool Ok = false;
            if (!ExprTruthy(s, Opbase, Ln.opCount, Ok)) return SCRIPT_ERR_OPERAND;
            if (!Ok) {
                s->waitingOnTime = false;
                s->State = (uint8_t)ScriptState::Waiting;
                Yield = true; // ic stays: re-evaluated on resume
                return SCRIPT_ERR_NONE;
            }
            s->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_TIME_GET: {
            if (Ln.destCount < 1) return SCRIPT_ERR_OPERAND;
            uint16_t Dtype = 0; uint8_t *Dest = nullptr; uint8_t Dsize = 0;
            if (!ResolveDest(s, s->Instr + (size_t)Ln.Start * 4, Dtype, Dest, Dsize)) return SCRIPT_ERR_OPERAND;
            // Time is a whole millisecond count. A Q16.16 Number destination only has a
            // 15-bit integer part (it overflows past ~32767 ms), so restrict Get time to
            // the integer types.
            if (Dtype != (uint16_t)DataType::Index && Dtype != (uint16_t)DataType::Uint32)
                return SCRIPT_ERR_TYPE;
            ScriptScalar v;
            v.i = (int32_t)nowMs;
            v.n = Number::FromRaw(0); // unused: integer destination
            StoreScalar(Dtype, Dest, Dsize, false, v);
            s->Ic++;
            return SCRIPT_ERR_NONE;
        }
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}

// Reads a 4-byte BlockInfo from a Constant operand.
inline bool ScriptService::OperandBlockInfo(LoadedScript *s, const uint8_t *Sym, uint32_t &Bi) {
    uint8_t Scratch[4];
    const uint8_t *Data = nullptr;
    uint8_t Size = 0;
    uint16_t Dtype = 0;
    if (!Resolve(s, Sym[0], Sym[1], SymVal(Sym), Scratch, &Data, &Size, &Dtype)) return false;
    if (Size < 4) return false;
    memcpy(&Bi, Data, 4);
    return true;
}

inline void ScriptService::SendRegisterRequest(LoadedScript *s, uint16_t Addr, uint8_t Cid,
                                      const uint8_t *Payload, uint16_t Len) {
    PacketFrame Req;
    PacketConstruct(&Req, Addr, MakeService(ServiceType::Register, Cid), s->Trid,
                    FLAG_REQACK | FLAG_START | FLAG_STOP, Payload, Len);
    SendAndVerifyPacket(Req);
}

inline uint8_t ScriptService::ExecService(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                                 uint8_t Op, uint32_t nowMs, bool &Yield) {
    switch (Op) {
        case SCRIPT_OP_SERVICE_NOP:
            s->Ic++;
            return SCRIPT_ERR_NONE;
        case SCRIPT_OP_SERVICE_LOG: {
            // Custom log (docs "Custom logs"): the first operand's value becomes the
            // log code; the source is the Script service tagged with the instance.
            uint16_t Code = (uint16_t)(s->Slot & 0xFF);
            if (Ln.opCount >= 1) {
                int32_t v = 0;
                if (ResolveOperandInt(s, Opbase, v)) Code = (uint16_t)(v & 0xFFFF);
            }
            // A custom log is an informational message, not an error report.
            ReportLog(MakeLog(false, (uint16_t)ServiceType::Script, Code, 0), PRIORITY_LOG);
            s->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_STATE: {
            if (Ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            if (Opbase[0] == SCRIPT_SYM_PREDEFINE && Opbase[1] == SCRIPT_PRE_STATE) {
                uint8_t St = (uint8_t)(SymVal(Opbase) & 0xFF);
                if (St > (uint8_t)ScriptState::Error) return SCRIPT_ERR_OPERAND;
                SetState(s, St);
                if (St == (uint8_t)ScriptState::Stopped || St == (uint8_t)ScriptState::Finished ||
                    St == (uint8_t)ScriptState::Error)
                    return SCRIPT_ERR_NONE; // terminal: the run loop stops here
                s->Ic++;
                return SCRIPT_ERR_NONE;
            }
            s->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_READ: { // dest = register[BlockInfo]
            if (Ln.destCount < 1 || Ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            uint32_t Bi = 0;
            if (!OperandBlockInfo(s, Opbase, Bi)) return SCRIPT_ERR_OPERAND;
            ValueInfo Rm;
            uint8_t Rbuf[256]; // a register value can be up to the u8 size limit
            uint8_t Rsz = 0;
            if (!RegisterGetByBlockInfo(Bi, Rm, Rbuf, Rsz)) return SCRIPT_ERR_REGISTER;
            uint8_t Err = Assign(s, s->Instr + (size_t)Ln.Start * 4, Rbuf, Rsz,
                                       ValueInfoType(Rm.Type));
            if (Err) return Err;
            s->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_WRITE: { // register[BlockInfo] = operand2
            if (Ln.opCount < 2) return SCRIPT_ERR_OPERAND;
            uint32_t Bi = 0;
            if (!OperandBlockInfo(s, Opbase, Bi)) return SCRIPT_ERR_OPERAND;
            ValueInfo Rm;
            uint8_t Rbuf[256];
            uint8_t Rsz = 0;
            if (!RegisterGetByBlockInfo(Bi, Rm, Rbuf, Rsz)) return SCRIPT_ERR_REGISTER;
            uint8_t Scratch[4];
            const uint8_t *Val = nullptr;
            uint8_t Vsize = 0;
            uint16_t Vtype = 0;
            if (!Resolve(s, Opbase[4], Opbase[5], SymVal(Opbase + 4), Scratch, &Val, &Vsize, &Vtype))
                return SCRIPT_ERR_OPERAND;
            uint8_t Wbuf[256];
            memset(Wbuf, 0, sizeof(Wbuf));
            uint8_t Err = AssignResolved(ValueInfoType(Rm.Type), Wbuf, Rm.Size, Val, Vsize, Vtype);
            if (Err) return Err;
            ValueInfo Wm = Rm;
            Wm.Size = Rm.Size;
            if (!RegisterSetByBlockInfo(Bi, Wm, Wbuf, Rm.Size)) return SCRIPT_ERR_REGISTER;
            s->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_READ_FOREIGN: { // dest = register[addr, BlockInfo]
            if (Ln.destCount < 1 || Ln.opCount < 2) return SCRIPT_ERR_OPERAND;
            int32_t Addr = 0;
            if (!ResolveOperandInt(s, Opbase, Addr)) return SCRIPT_ERR_OPERAND;
            uint32_t Bi = 0;
            if (!OperandBlockInfo(s, Opbase + 4, Bi)) return SCRIPT_ERR_OPERAND;
            memcpy(s->pendingDest, s->Instr + (size_t)Ln.Start * 4, 4);
            s->pendingRead = true;
            s->pendingForeign = true;
            s->pendingTrid = s->Trid;
            s->pendingDeadline = nowMs + 500;
            s->Ic++;
            s->State = (uint8_t)ScriptState::Waiting;
            s->waitingOnTime = false;
            Yield = true;
            uint8_t Payload[4];
            memcpy(Payload, &Bi, 4);
            SendRegisterRequest(s, (uint16_t)Addr, 1, Payload, 4);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_WRITE_FOREIGN: { // register[addr, BlockInfo] = operand3
            if (Ln.opCount < 3) return SCRIPT_ERR_OPERAND;
            int32_t Addr = 0;
            if (!ResolveOperandInt(s, Opbase, Addr)) return SCRIPT_ERR_OPERAND;
            uint32_t Bi = 0;
            if (!OperandBlockInfo(s, Opbase + 4, Bi)) return SCRIPT_ERR_OPERAND;
            uint8_t Scratch[4];
            const uint8_t *Val = nullptr;
            uint8_t Vsize = 0;
            uint16_t Vtype = 0;
            if (!Resolve(s, Opbase[8], Opbase[9], SymVal(Opbase + 8), Scratch, &Val, &Vsize, &Vtype))
                return SCRIPT_ERR_OPERAND;
            if (Vsize > 64) Vsize = 64;
            uint8_t Payload[8 + 64];
            memcpy(Payload, &Bi, 4);
            ValueInfo Wm = { (uint16_t)Vtype, Vsize, 0 };
            memcpy(Payload + 4, &Wm, 4);
            if (Vsize) memcpy(Payload + 8, Val, Vsize);
            s->pendingRead = false;
            s->pendingForeign = true;
            s->pendingTrid = s->Trid;
            s->pendingDeadline = nowMs + 500;
            s->Ic++;
            s->State = (uint8_t)ScriptState::Waiting;
            s->waitingOnTime = false;
            Yield = true;
            SendRegisterRequest(s, (uint16_t)Addr, 2, Payload, (uint16_t)(8 + Vsize));
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_SCRIPT_LOAD: { // load(script file id, loaded script id)
            // Mirrors management CID 1: the script picks the slot and the pair may differ.
            if (Ln.opCount < 2) return SCRIPT_ERR_OPERAND;
            int32_t fileId = 0, Slot = 0;
            if (!ResolveOperandInt(s, Opbase, fileId)) return SCRIPT_ERR_OPERAND;
            if (!ResolveOperandInt(s, Opbase + 4, Slot)) return SCRIPT_ERR_OPERAND;
            // Loading over the running script would free the program currently being executed.
            if (Slot == (int32_t)s->Slot) return SCRIPT_ERR_OPERAND;
            if (fileId >= 0 && fileId < MAX_SCRIPT_FILES && Slot >= 0 && Slot < MAX_SCRIPTS)
                Load((uint16_t)fileId, (uint8_t)Slot);
            s->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_SCRIPT_UNLOAD: { // unload(loaded script id)
            // Same operation as management CID 2. A script that is not loaded is a no-op, like
            // that CID reporting no work rather than failing.
            if (Ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t Slot = 0;
            if (!ResolveOperandInt(s, Opbase, Slot)) return SCRIPT_ERR_OPERAND;
            // Never unload the running script: its program and spaces are what is executing.
            if (Slot == (int32_t)s->Slot) return SCRIPT_ERR_OPERAND;
            if (Slot >= 0 && Slot < MAX_SCRIPTS && Active((uint8_t)Slot)) Unload((uint8_t)Slot);
            s->Ic++;
            return SCRIPT_ERR_NONE;
        }
        default:
            return SCRIPT_ERR_NOT_IMPL;
    }
}

// Resolves one element of a vector/matrix/colour/string container.
inline bool ScriptService::ContainerElement(LoadedScript *s, const uint8_t *Sym, int32_t Index,
                                   uint8_t *Scratch, const uint8_t **Elem, uint8_t *elemSize,
                                   uint16_t *elemType) {
    const uint8_t *Data = nullptr;
    uint8_t Size = 0;
    uint16_t Dtype = 0;
    if (!Resolve(s, Sym[0], Sym[1], SymVal(Sym), Scratch, &Data, &Size, &Dtype)) return false;
    switch (Dtype) {
        case (uint16_t)DataType::Vector:
            if (Index < 0 || (uint32_t)(Index + 1) * 4 > Size) return false;
            *Elem = Data + (size_t)Index * 4;
            *elemSize = 4;
            *elemType = (uint16_t)DataType::Number;
            return true;
        case (uint16_t)DataType::Matrix: {
            if (Size < 4 || Index < 0) return false;
            uint16_t h = (uint16_t)(Data[0] | (Data[1] << 8));
            uint16_t w = (uint16_t)(Data[2] | (Data[3] << 8));
            if ((uint32_t)Index >= (uint32_t)h * w) return false;
            if (4u + (uint32_t)(Index + 1) * 4 > Size) return false;
            *Elem = Data + 4 + (size_t)Index * 4;
            *elemSize = 4;
            *elemType = (uint16_t)DataType::Number;
            return true;
        }
        case (uint16_t)DataType::Colour:
            if (Index < 0 || Index > 3 || Size < 4) return false;
            *Elem = Data + Index;
            *elemSize = 1;
            *elemType = (uint16_t)DataType::Uint32;
            return true;
        case (uint16_t)DataType::Text:
            if (Index < 0 || Index >= Size) return false;
            *Elem = Data + Index;
            *elemSize = 1;
            *elemType = (uint16_t)DataType::Char;
            return true;
        default:
            if (Index != 0) return false;
            *Elem = Data;
            *elemSize = Size;
            *elemType = Dtype;
            return true;
    }
}

// Compose: container[index...] = value...   Extract: dest = container[index]
inline uint8_t ScriptService::ExecCompose(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op) {
    if (Op == SCRIPT_OP_COMPOSE) {
        if (Ln.destCount < 1 || Ln.opCount < 2) return SCRIPT_ERR_OPERAND;
        const uint8_t *destSym = s->Instr + (size_t)Ln.Start * 4;
        // The container must be writable (a variable or an output).
        if (destSym[0] != SCRIPT_SYM_VARIABLE && destSym[0] != SCRIPT_SYM_OUTPUT)
            return SCRIPT_ERR_OPERAND;
        int32_t Index = 0;
        if (!ResolveOperandInt(s, Opbase, Index)) return SCRIPT_ERR_OPERAND;
        for (uint8_t k = 1; k < Ln.opCount; k++) {
            uint8_t Vscratch[4];
            const uint8_t *Val = nullptr;
            uint8_t Vsize = 0;
            uint16_t Vtype = 0;
            if (!Resolve(s, Opbase[k * 4], Opbase[k * 4 + 1], SymVal(Opbase + (size_t)k * 4),
                               Vscratch, &Val, &Vsize, &Vtype))
                return SCRIPT_ERR_OPERAND;
            const uint8_t *Elem = nullptr;
            uint8_t Esize = 0;
            uint16_t Etype = 0;
            // Separate scratch: `val` may point into `vscratch`.
            uint8_t Escratch[4];
            if (!ContainerElement(s, destSym, Index + (k - 1), Escratch, &Elem, &Esize, &Etype))
                return SCRIPT_ERR_BOUNDS;
            uint8_t Err = AssignResolved(Etype, const_cast<uint8_t *>(Elem), Esize, Val, Vsize, Vtype);
            if (Err) return Err;
        }
        s->Ic++;
        return SCRIPT_ERR_NONE;
    }
    if (Op == SCRIPT_OP_EXTRACT) {
        if (Ln.destCount < 1 || Ln.opCount < 2) return SCRIPT_ERR_OPERAND;
        int32_t Index = 0;
        if (!ResolveOperandInt(s, Opbase + 4, Index)) return SCRIPT_ERR_OPERAND;
        uint8_t Scratch[4];
        const uint8_t *Elem = nullptr;
        uint8_t Esize = 0;
        uint16_t Etype = 0;
        if (!ContainerElement(s, Opbase, Index, Scratch, &Elem, &Esize, &Etype)) return SCRIPT_ERR_BOUNDS;
        uint8_t Err = Assign(s, s->Instr + (size_t)Ln.Start * 4, Elem, Esize, Etype);
        if (Err) return Err;
        s->Ic++;
        return SCRIPT_ERR_NONE;
    }
    return SCRIPT_ERR_UNKNOWN_OP;
}

inline uint8_t ScriptService::ExecLine(LoadedScript *s, uint32_t nowMs, bool &Yield) {
    const ScriptLineInfo &Ln = s->Lines[s->Ic];
    const uint8_t *Instr = s->Instr + (size_t)(Ln.Start + Ln.destCount) * 4;
    uint8_t Cat = Instr[1];
    uint8_t Op = Instr[2];
    const uint8_t *Opbase = Instr + 4;
    switch (Cat) {
        case SCRIPT_CAT_MATH:
        case SCRIPT_CAT_LOGIC:
            return ExecMath(s, Ln, Opbase, Cat, Op);
        case SCRIPT_CAT_FLOW:
            return ExecFlow(s, Ln, Opbase, Op);
        case SCRIPT_CAT_TIME:
            return ExecTime(s, Ln, Opbase, Op, nowMs, Yield);
        case SCRIPT_CAT_SERVICE:
            return ExecService(s, Ln, Opbase, Op, nowMs, Yield);
        case SCRIPT_CAT_COMPOSE:
            return ExecCompose(s, Ln, Opbase, Op);
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}


#endif // USE_SCRIPTS
