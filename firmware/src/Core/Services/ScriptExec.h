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
static uint8_t ScriptExecSet(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                             uint16_t dtype, uint8_t *dest, uint8_t dsize)
{
    if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
    ExprParser p;
    p.s = s;
    p.tok = opbase;
    p.n = ln.opCount;
    p.i = 0;
    p.depth = 0;
    ExprValue v;
    if (!ScriptExprBin(&p, v, 1)) return SCRIPT_ERR_OPERAND;
    if (p.i != p.n) return SCRIPT_ERR_OPERAND; // trailing tokens

    if (dtype == (uint16_t)DataType::Vector)
    {
        uint8_t dcount = dsize / 4;
        if (dcount == 0 || v.count != dcount) return SCRIPT_ERR_TYPE;
        for (uint8_t k = 0; k < dcount; k++)
        {
            int32_t raw = v.e[k].Value;
            memcpy(dest + (size_t)k * 4, &raw, 4);
        }
    }
    else if (dtype == (uint16_t)DataType::Matrix)
    {
        // A Matrix destination takes its dimensions from the value's header (the dest slot
        // may be uninitialised). The value must be a Matrix (or a Matrix expression result).
        if (v.dtype != (uint16_t)DataType::Matrix || v.count == 0) return SCRIPT_ERR_TYPE;
        if ((uint32_t)dsize < 4u + (uint32_t)v.count * 4u) return SCRIPT_ERR_TYPE;
        dest[0] = (uint8_t)(v.mh & 0xFF);
        dest[1] = (uint8_t)(v.mh >> 8);
        dest[2] = (uint8_t)(v.mw & 0xFF);
        dest[3] = (uint8_t)(v.mw >> 8);
        for (uint8_t k = 0; k < v.count; k++)
        {
            int32_t raw = v.e[k].Value;
            memcpy(dest + 4 + (size_t)k * 4, &raw, 4);
        }
    }
    else
    {
        if (v.count != 1) return SCRIPT_ERR_TYPE;
        ScriptScalar sc;
        sc.n = v.e[0];
        sc.i = v.e[0].RoundToInt();
        ScriptStoreScalar(dtype, dest, dsize, dtype == (uint16_t)DataType::Number, sc);
    }
    s->ic++;
    return SCRIPT_ERR_NONE;
}

// Evaluates a token stream as an expression and returns its truth value (used by the flow
// instructions and Wait until).
static bool ScriptExprTruthy(LoadedScript *s, const uint8_t *tok, uint8_t n, bool &ok)
{
    if (n < 1) return false;
    ExprParser p;
    p.s = s;
    p.tok = tok;
    p.n = n;
    p.i = 0;
    p.depth = 0;
    ExprValue v;
    if (!ScriptExprBin(&p, v, 1)) return false;
    if (p.i != p.n) return false;
    ok = (v.count > 0) && (v.e[0].Value != 0);
    return true;
}

// Transform dest = rot, ox, oy, sx, sy [, skew] -> a 2x3 affine matrix (rotation in
// radians), matching the render Position/Offset format.
static uint8_t ScriptExecTransform(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                                   uint16_t dtype, uint8_t *dest, uint8_t dsize)
{
    if (dtype != (uint16_t)DataType::Matrix || dsize < 4 + 6 * 4) return SCRIPT_ERR_TYPE;
    if (ln.opCount < 5) return SCRIPT_ERR_OPERAND;
    Number v[6] = {Number(0), Number(0), Number(0), Number(0), Number(0), Number(0)};
    uint8_t n = ln.opCount;
    if (n > 6) n = 6;
    for (uint8_t i = 0; i < n; i++)
    {
        ScriptScalar sc;
        if (!ScriptResolveOperandScalar(s, opbase + (size_t)i * 4, true, sc)) return SCRIPT_ERR_OPERAND;
        v[i] = sc.n;
    }
    Number rot = v[0], tx = v[1], ty = v[2], sx = v[3], sy = v[4], skew = v[5];
    Number c = cos(rot), s2 = sin(rot);
    Number k = Number(0);
    if (skew.Value != 0)
    {
        Number cd = cos(skew);
        if (cd.Value != 0) k = sin(skew) / cd;
    }
    // Cells [a b tx; c d ty] (matches the app's Transform23).
    Number cells[6];
    cells[0] = sx * c;
    cells[1] = sx * c * k + sy * s2;
    cells[2] = tx;
    cells[3] = -sx * s2;
    cells[4] = -sx * s2 * k + sy * c;
    cells[5] = ty;
    // The renderer samples the geometry mask forward (pp = Position * coord), so the shape's
    // centre lands where the sampled point is zero: Position^-1(0) = -L^-1 * t. To keep the
    // centre where the caller asked for it (-t, the same as an unrotated translation) the
    // stored translation is pre-rotated by the linear part: t' = L * t.
    Number txr = cells[0] * tx + cells[1] * ty;
    Number tyr = cells[3] * tx + cells[4] * ty;
    cells[2] = txr;
    cells[5] = tyr;
    dest[0] = 2; dest[1] = 0; dest[2] = 3; dest[3] = 0;
    for (uint8_t i = 0; i < 6; i++)
    {
        int32_t raw = cells[i].Value;
        memcpy(dest + 4 + (size_t)i * 4, &raw, 4);
    }
    s->ic++;
    return SCRIPT_ERR_NONE;
}

static uint8_t ScriptExecMath(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t cat, uint8_t op) {
    if (ln.destCount < 1) return SCRIPT_ERR_OPERAND;
    uint16_t dtype = 0;
    uint8_t *dest = nullptr;
    uint8_t dsize = 0;
    if (!ScriptResolveDest(s, s->instr + (size_t)ln.start * 4, dtype, dest, dsize)) return SCRIPT_ERR_OPERAND;
    bool num = (dtype == (uint16_t)DataType::Number);

    // SET evaluates an infix expression (scalar / vector / matrix).
    if (cat == SCRIPT_CAT_MATH && op == SCRIPT_OP_MATH_SET) {
        return ScriptExecSet(s, ln, opbase, dtype, dest, dsize);
    }

    // Transform: a 2x3 affine matrix from (rot, ox, oy, sx, sy[, skew]).
    if (cat == SCRIPT_CAT_MATH && op == SCRIPT_OP_MATH_TRANSFORM) {
        return ScriptExecTransform(s, ln, opbase, dtype, dest, dsize);
    }

    // Vector/Matrix destination: element-wise math (operands resolved raw).
    if (dtype == (uint16_t)DataType::Vector || dtype == (uint16_t)DataType::Matrix) {
        return ScriptExecVectorMath(s, ln, opbase, cat, op, dtype, dest, dsize);
    }

    ScriptScalar in[8];
    uint8_t n = ln.opCount;
    if (n > 8) n = 8;
    if (n < 1) return SCRIPT_ERR_OPERAND;
    for (uint8_t i = 0; i < n; i++) {
        if (!ScriptResolveOperandScalar(s, opbase + (size_t)i * 4, num, in[i])) return SCRIPT_ERR_OPERAND;
    }

    ScriptScalar out;
    out.n = Number(0);
    out.i = 0;
    switch (cat) {
        case SCRIPT_CAT_MATH:
            // Fold every operand left-to-right (n-ary: dest = a op b op c ...).
            out = in[0];
            switch (op) {
                case SCRIPT_OP_MATH_MOD:
                    for (uint8_t k = 1; k < n; k++) {
                        if (num) { int32_t b = in[k].n.RoundToInt(); if (b == 0) return SCRIPT_ERR_TYPE; out.n = Number(out.n.RoundToInt() % b); }
                        else { if (in[k].i == 0) return SCRIPT_ERR_TYPE; out.i %= in[k].i; }
                    }
                    break;
                case SCRIPT_OP_MATH_MIN:
                    for (uint8_t k = 1; k < n; k++) { if (num) out.n = min(out.n, in[k].n); else out.i = out.i < in[k].i ? out.i : in[k].i; }
                    break;
                case SCRIPT_OP_MATH_MAX:
                    for (uint8_t k = 1; k < n; k++) { if (num) out.n = max(out.n, in[k].n); else out.i = out.i > in[k].i ? out.i : in[k].i; }
                    break;
                case SCRIPT_OP_MATH_ABS: if (num) out.n = abs(out.n); else out.i = out.i < 0 ? -out.i : out.i; break;
                case SCRIPT_OP_MATH_LIMIT: {
                    if (n < 3) return SCRIPT_ERR_OPERAND;
                    if (num) {
                        Number lo = in[1].n, hi = in[2].n;
                        if (out.n.Value < lo.Value) out.n = lo;
                        if (out.n.Value > hi.Value) out.n = hi;
                    } else {
                        int32_t lo = in[1].i, hi = in[2].i;
                        if (out.i < lo) out.i = lo;
                        if (out.i > hi) out.i = hi;
                    }
                    break;
                }
                default: return SCRIPT_ERR_UNKNOWN_OP;
            }
            break;
        case SCRIPT_CAT_LOGIC:
            // Comparisons/logic moved into the expression (Set/If/While); only Select remains.
            switch (op) {
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

static uint8_t ScriptExecFlow(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t op) {
    switch (op) {
        case SCRIPT_OP_FLOW_IF: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool ok = false;
            if (!ScriptExprTruthy(s, opbase, ln.opCount, ok)) return SCRIPT_ERR_OPERAND;
            uint16_t match = s->blockMatch[s->ic];
            s->ic = ok ? s->ic + 1 : (match == 0xFFFF ? s->ic + 1 : match + 1);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_WHILE: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool ok = false;
            if (!ScriptExprTruthy(s, opbase, ln.opCount, ok)) return SCRIPT_ERR_OPERAND;
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
            if (!ScriptExprTruthy(s, opbase, ln.opCount, ok)) return SCRIPT_ERR_OPERAND;
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
            uint16_t dtype = 0; uint8_t *dest = nullptr; uint8_t dsize = 0;
            if (!ScriptResolveDest(s, s->instr + (size_t)ln.start * 4, dtype, dest, dsize)) return SCRIPT_ERR_OPERAND;
            // Time is a whole millisecond count. A Q16.16 Number destination only has a
            // 15-bit integer part (it overflows past ~32767 ms), so restrict Get time to
            // the integer types.
            if (dtype != (uint16_t)DataType::Index && dtype != (uint16_t)DataType::Uint32)
                return SCRIPT_ERR_TYPE;
            ScriptScalar v;
            v.i = (int32_t)nowMs;
            v.n = Number::FromRaw(0); // unused: integer destination
            ScriptStoreScalar(dtype, dest, dsize, false, v);
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
        case SCRIPT_OP_SERVICE_LOG: {
            // Custom log (docs "Custom logs"): the first operand's value becomes the
            // log code; the source is the Script service tagged with the instance.
            uint16_t code = (uint16_t)(s->slot & 0xFF);
            if (ln.opCount >= 1) {
                int32_t v = 0;
                if (ScriptResolveOperandInt(s, opbase, v)) code = (uint16_t)(v & 0xFFFF);
            }
            ReportLog(MakeLog(false, (uint16_t)ServiceType::Script, code, 0));
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_STATE: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            if (opbase[0] == SCRIPT_SYM_PREDEFINE && opbase[1] == SCRIPT_PRE_STATE) {
                uint8_t st = (uint8_t)(ScriptSymVal(opbase) & 0xFF);
                if (st > (uint8_t)ScriptState::Error) return SCRIPT_ERR_OPERAND;
                ScriptSetState(s, st);
                if (st == (uint8_t)ScriptState::Stopped || st == (uint8_t)ScriptState::Finished ||
                    st == (uint8_t)ScriptState::Error)
                    return SCRIPT_ERR_NONE; // terminal: the run loop stops here
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
            ValueInfo rm;
            uint8_t rbuf[256]; // a register value can be up to the u8 size limit
            uint8_t rsz = 0;
            if (!RegisterGetByBlockInfo(bi, rm, rbuf, rsz)) return SCRIPT_ERR_REGISTER;
            uint8_t err = ScriptAssign(s, s->instr + (size_t)ln.start * 4, rbuf, rsz,
                                       ValueInfoType(rm.Type));
            if (err) return err;
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_WRITE: { // register[BlockInfo] = operand2
            if (ln.opCount < 2) return SCRIPT_ERR_OPERAND;
            uint32_t bi = 0;
            if (!ScriptOperandBlockInfo(s, opbase, bi)) return SCRIPT_ERR_OPERAND;
            ValueInfo rm;
            uint8_t rbuf[256];
            uint8_t rsz = 0;
            if (!RegisterGetByBlockInfo(bi, rm, rbuf, rsz)) return SCRIPT_ERR_REGISTER;
            uint8_t scratch[4];
            const uint8_t *val = nullptr;
            uint8_t vsize = 0;
            uint16_t vtype = 0;
            if (!ScriptResolve(s, opbase[4], opbase[5], ScriptSymVal(opbase + 4), scratch, &val, &vsize, &vtype))
                return SCRIPT_ERR_OPERAND;
            uint8_t wbuf[256];
            memset(wbuf, 0, sizeof(wbuf));
            uint8_t err = ScriptAssignResolved(ValueInfoType(rm.Type), wbuf, rm.Size, val, vsize, vtype);
            if (err) return err;
            ValueInfo wm = rm;
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
            ValueInfo wm = { (uint16_t)vtype, vsize, 0 };
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
        case SCRIPT_OP_SERVICE_SCRIPT_LOAD: { // load(script file id, loaded script id)
            // Mirrors management CID 1: the script picks the slot and the pair may differ.
            if (ln.opCount < 2) return SCRIPT_ERR_OPERAND;
            int32_t fileId = 0, slot = 0;
            if (!ScriptResolveOperandInt(s, opbase, fileId)) return SCRIPT_ERR_OPERAND;
            if (!ScriptResolveOperandInt(s, opbase + 4, slot)) return SCRIPT_ERR_OPERAND;
            // Loading over the running script would free the program currently being executed.
            if (slot == (int32_t)s->slot) return SCRIPT_ERR_OPERAND;
            if (fileId >= 0 && fileId < MAX_SCRIPT_FILES && slot >= 0 && slot < MAX_SCRIPTS)
                ScriptLoad((uint16_t)fileId, (uint8_t)slot);
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_SCRIPT_UNLOAD: { // unload(loaded script id)
            // Same operation as management CID 2. A script that is not loaded is a no-op, like
            // that CID reporting no work rather than failing.
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t slot = 0;
            if (!ScriptResolveOperandInt(s, opbase, slot)) return SCRIPT_ERR_OPERAND;
            // Never unload the running script: its program and spaces are what is executing.
            if (slot == (int32_t)s->slot) return SCRIPT_ERR_OPERAND;
            if (slot >= 0 && slot < MAX_SCRIPTS && ScriptActive((uint8_t)slot)) ScriptUnload((uint8_t)slot);
            s->ic++;
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
        // The container must be writable (a variable or an output).
        if (destSym[0] != SCRIPT_SYM_VARIABLE && destSym[0] != SCRIPT_SYM_OUTPUT)
            return SCRIPT_ERR_OPERAND;
        int32_t index = 0;
        if (!ScriptResolveOperandInt(s, opbase, index)) return SCRIPT_ERR_OPERAND;
        for (uint8_t k = 1; k < ln.opCount; k++) {
            uint8_t vscratch[4];
            const uint8_t *val = nullptr;
            uint8_t vsize = 0;
            uint16_t vtype = 0;
            if (!ScriptResolve(s, opbase[k * 4], opbase[k * 4 + 1], ScriptSymVal(opbase + (size_t)k * 4),
                               vscratch, &val, &vsize, &vtype))
                return SCRIPT_ERR_OPERAND;
            const uint8_t *elem = nullptr;
            uint8_t esize = 0;
            uint16_t etype = 0;
            // Separate scratch: `val` may point into `vscratch`.
            uint8_t escratch[4];
            if (!ScriptContainerElement(s, destSym, index + (k - 1), escratch, &elem, &esize, &etype))
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


#endif // USE_SCRIPTS
