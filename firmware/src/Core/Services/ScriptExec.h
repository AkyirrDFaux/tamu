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
inline uint8_t ScriptService::ExecSet(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                             uint16_t Dtype, uint8_t *Dest, uint8_t Dsize)
{
    if (Ln.OpCount < 1) return SCRIPT_ERR_OPERAND;
    ExprParser Parser;
    Parser.Script = Script;
    Parser.Tok = Opbase;
    Parser.Count = Ln.OpCount;
    Parser.Index = 0;
    Parser.Depth = 0;
    ExprValue Expr;
    if (!ExprBin(&Parser, Expr, 1)) return SCRIPT_ERR_OPERAND;
    if (Parser.Index != Parser.Count) return SCRIPT_ERR_OPERAND; // trailing tokens

    if (Dtype == (uint16_t)DataType::Vector)
    {
        uint8_t Dcount = Dsize / 4;
        if (Dcount == 0 || Expr.Count != Dcount) return SCRIPT_ERR_TYPE;
        for (uint8_t k = 0; k < Dcount; k++)
        {
            int32_t Raw = Expr.Elems[k].Value;
            memcpy(Dest + (size_t)k * 4, &Raw, 4);
        }
    }
    else if (Dtype == (uint16_t)DataType::Matrix)
    {
        // A Matrix destination takes its dimensions from the value's header (the dest slot
        // may be uninitialised). The value must be a Matrix (or a Matrix expression result).
        if (Expr.Dtype != (uint16_t)DataType::Matrix || Expr.Count == 0) return SCRIPT_ERR_TYPE;
        if ((uint32_t)Dsize < 4u + (uint32_t)Expr.Count * 4u) return SCRIPT_ERR_TYPE;
        Dest[0] = (uint8_t)(Expr.Mh & 0xFF);
        Dest[1] = (uint8_t)(Expr.Mh >> 8);
        Dest[2] = (uint8_t)(Expr.Mw & 0xFF);
        Dest[3] = (uint8_t)(Expr.Mw >> 8);
        for (uint8_t k = 0; k < Expr.Count; k++)
        {
            int32_t Raw = Expr.Elems[k].Value;
            memcpy(Dest + 4 + (size_t)k * 4, &Raw, 4);
        }
    }
    else
    {
        if (Expr.Count != 1) return SCRIPT_ERR_TYPE;
        ScriptScalar Sc;
        Sc.Num = Expr.Elems[0];
        Sc.Int = Expr.Elems[0].RoundToInt();
        StoreScalar(Dtype, Dest, Dsize, Dtype == (uint16_t)DataType::Number, Sc);
    }
    Script->Ic++;
    return SCRIPT_ERR_NONE;
}

// Evaluates a token stream as an expression and returns its truth value (used by the flow
// instructions and Wait until).
inline bool ScriptService::ExprTruthy(LoadedScript *Script, const uint8_t *Tok, uint8_t Count, bool &Ok)
{
    if (Count < 1) return false;
    ExprParser Parser;
    Parser.Script = Script;
    Parser.Tok = Tok;
    Parser.Count = Count;
    Parser.Index = 0;
    Parser.Depth = 0;
    ExprValue Expr;
    if (!ExprBin(&Parser, Expr, 1)) return false;
    if (Parser.Index != Parser.Count) return false;
    Ok = (Expr.Count > 0) && (Expr.Elems[0].Value != 0);
    return true;
}

// Transform dest = rot, ox, oy, sx, sy [, skew] -> a 2x3 affine matrix (rotation in
// radians), matching the render Position/Offset format.
inline uint8_t ScriptService::ExecTransform(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                                   uint16_t Dtype, uint8_t *Dest, uint8_t Dsize)
{
    if (Dtype != (uint16_t)DataType::Matrix || Dsize < 4 + 6 * 4) return SCRIPT_ERR_TYPE;
    if (Ln.OpCount < 5) return SCRIPT_ERR_OPERAND;
    Number Vals[6] = {Number(0), Number(0), Number(0), Number(0), Number(0), Number(0)};
    uint8_t Count = Ln.OpCount;
    if (Count > 6) Count = 6;
    for (uint8_t i = 0; i < Count; i++)
    {
        ScriptScalar Sc;
        if (!ResolveOperandScalar(Script, Opbase + (size_t)i * 4, true, Sc)) return SCRIPT_ERR_OPERAND;
        Vals[i] = Sc.Num;
    }
    Number Rot = Vals[0], Tx = Vals[1], Ty = Vals[2], Sx = Vals[3], Sy = Vals[4], Skew = Vals[5];
    Number Cos = cos(Rot), S2 = sin(Rot);
    Number k = Number(0);
    if (Skew.Value != 0)
    {
        Number Cd = cos(Skew);
        if (Cd.Value != 0) k = sin(Skew) / Cd;
    }
    // Cells [a b tx; c d ty] (matches the app's Transform23).
    Number Cells[6];
    Cells[0] = Sx * Cos;
    Cells[1] = Sx * Cos * k + Sy * S2;
    Cells[2] = Tx;
    Cells[3] = -Sx * S2;
    Cells[4] = -Sx * S2 * k + Sy * Cos;
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
    Script->Ic++;
    return SCRIPT_ERR_NONE;
}

inline uint8_t ScriptService::ExecMath(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Cat, uint8_t Op) {
    if (Ln.DestCount < 1) return SCRIPT_ERR_OPERAND;
    uint16_t Dtype = 0;
    uint8_t *Dest = nullptr;
    uint8_t Dsize = 0;
    if (!ResolveDest(Script, Script->Instr + (size_t)Ln.Start * 4, Dtype, Dest, Dsize)) return SCRIPT_ERR_OPERAND;
    bool Num = (Dtype == (uint16_t)DataType::Number);

    // SET evaluates an infix expression (scalar / vector / matrix).
    if (Cat == SCRIPT_CAT_MATH && Op == SCRIPT_OP_MATH_SET) {
        return ExecSet(Script, Ln, Opbase, Dtype, Dest, Dsize);
    }

    // Transform: a 2x3 affine matrix from (rot, ox, oy, sx, sy[, skew]).
    if (Cat == SCRIPT_CAT_MATH && Op == SCRIPT_OP_MATH_TRANSFORM) {
        return ExecTransform(Script, Ln, Opbase, Dtype, Dest, Dsize);
    }

    // Vector/Matrix destination: element-wise math (operands resolved raw).
    if (Dtype == (uint16_t)DataType::Vector || Dtype == (uint16_t)DataType::Matrix) {
        return ExecVectorMath(Script, Ln, Opbase, Cat, Op, Dtype, Dest, Dsize);
    }

    ScriptScalar In[8];
    uint8_t Count = Ln.OpCount;
    if (Count > 8) Count = 8;
    if (Count < 1) return SCRIPT_ERR_OPERAND;
    for (uint8_t i = 0; i < Count; i++) {
        if (!ResolveOperandScalar(Script, Opbase + (size_t)i * 4, Num, In[i])) return SCRIPT_ERR_OPERAND;
    }

    ScriptScalar Out;
    Out.Num = Number(0);
    Out.Int = 0;
    switch (Cat) {
        case SCRIPT_CAT_MATH:
            // Fold every operand left-to-right (n-ary: dest = a op b op c ...).
            Out = In[0];
            switch (Op) {
                case SCRIPT_OP_MATH_MOD:
                    for (uint8_t k = 1; k < Count; k++) {
                        if (Num) { int32_t Divisor = In[k].Num.RoundToInt(); if (Divisor == 0) return SCRIPT_ERR_TYPE; Out.Num = Number(Out.Num.RoundToInt() % Divisor); }
                        else { if (In[k].Int == 0) return SCRIPT_ERR_TYPE; Out.Int %= In[k].Int; }
                    }
                    break;
                case SCRIPT_OP_MATH_MIN:
                    for (uint8_t k = 1; k < Count; k++) { if (Num) Out.Num = min(Out.Num, In[k].Num); else Out.Int = Out.Int < In[k].Int ? Out.Int : In[k].Int; }
                    break;
                case SCRIPT_OP_MATH_MAX:
                    for (uint8_t k = 1; k < Count; k++) { if (Num) Out.Num = max(Out.Num, In[k].Num); else Out.Int = Out.Int > In[k].Int ? Out.Int : In[k].Int; }
                    break;
                case SCRIPT_OP_MATH_ABS: if (Num) Out.Num = abs(Out.Num); else Out.Int = Out.Int < 0 ? -Out.Int : Out.Int; break;
                case SCRIPT_OP_MATH_LIMIT: {
                    if (Count < 3) return SCRIPT_ERR_OPERAND;
                    if (Num) {
                        Number Lo = In[1].Num, Hi = In[2].Num;
                        if (Out.Num.Value < Lo.Value) Out.Num = Lo;
                        if (Out.Num.Value > Hi.Value) Out.Num = Hi;
                    } else {
                        int32_t Lo = In[1].Int, Hi = In[2].Int;
                        if (Out.Int < Lo) Out.Int = Lo;
                        if (Out.Int > Hi) Out.Int = Hi;
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
                    if (Count < 3) return SCRIPT_ERR_OPERAND;
                    bool Cond = Num ? (In[0].Num.Value != 0) : (In[0].Int != 0);
                    if (Num) Out.Num = Cond ? In[1].Num : In[2].Num; else Out.Int = Cond ? In[1].Int : In[2].Int;
                    break;
                }
                default: return SCRIPT_ERR_UNKNOWN_OP;
            }
            break;
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }

    StoreScalar(Dtype, Dest, Dsize, Num, Out);
    Script->Ic++;
    return SCRIPT_ERR_NONE;
}

inline uint8_t ScriptService::ExecFlow(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op) {
    switch (Op) {
        // While is a byte-for-byte twin of If: both evaluate the condition and jump past the
        // block when it is false (an END of kind 2 returns to a While's line).
        case SCRIPT_OP_FLOW_IF:
        case SCRIPT_OP_FLOW_WHILE: {
            if (Ln.OpCount < 1) return SCRIPT_ERR_OPERAND;
            bool Ok = false;
            if (!ExprTruthy(Script, Opbase, Ln.OpCount, Ok)) return SCRIPT_ERR_OPERAND;
            uint16_t Match = Script->BlockMatch[Script->Ic];
            Script->Ic = Ok ? Script->Ic + 1 : (Match == 0xFFFF ? Script->Ic + 1 : Match + 1);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_END: {
            uint16_t Open = (Script->BlockMatch[Script->Ic] == 0xFFFF) ? 0xFFFF : Script->BlockMatch[Script->Ic];
            if (Open != 0xFFFF && Script->BlockKind[Open] == 2) { Script->Ic = Open; return SCRIPT_ERR_NONE; } // while -> re-check
            Script->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_JUMP: {
            if (Ln.OpCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t Target = 0;
            if (!ResolveOperandInt(Script, Opbase, Target)) return SCRIPT_ERR_OPERAND;
            if (Target < 0 || Target >= Script->LineCount) return SCRIPT_ERR_BOUNDS;
            Script->Ic = (uint16_t)Target;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_CALL: {
            if (Ln.OpCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t Target = 0;
            if (!ResolveOperandInt(Script, Opbase, Target)) return SCRIPT_ERR_OPERAND;
            if (Target < 0 || Target >= Script->LineCount) return SCRIPT_ERR_BOUNDS;
            if (Script->CallDepth >= SCRIPT_MAX_CALL_DEPTH) return SCRIPT_ERR_STACK;
            Script->CallStack[Script->CallDepth++] = (uint16_t)(Script->Ic + 1);
            Script->Ic = (uint16_t)Target;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_RETURN: {
            if (Script->CallDepth == 0) { Script->State = (uint8_t)ScriptState::Finished; return SCRIPT_ERR_NONE; }
            Script->Ic = Script->CallStack[--Script->CallDepth];
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_HALT:
            Script->State = (uint8_t)ScriptState::Finished;
            return SCRIPT_ERR_NONE;
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}

inline uint8_t ScriptService::ExecTime(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op, uint32_t NowMs, bool &Yield) {
    switch (Op) {
        case SCRIPT_OP_TIME_DELAY: {
            if (Ln.OpCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t Ms = 0;
            if (!ResolveOperandInt(Script, Opbase, Ms)) return SCRIPT_ERR_OPERAND;
            if (Ms < 0) Ms = 0;
            Script->Ic++;
            Script->WaitUntil = NowMs + (uint32_t)Ms;
            Script->WaitingOnTime = true;
            Script->State = (uint8_t)ScriptState::Waiting;
            Yield = true;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_TIME_WAIT: {
            if (Ln.OpCount < 1) return SCRIPT_ERR_OPERAND;
            bool Ok = false;
            if (!ExprTruthy(Script, Opbase, Ln.OpCount, Ok)) return SCRIPT_ERR_OPERAND;
            if (!Ok) {
                Script->WaitingOnTime = false;
                Script->State = (uint8_t)ScriptState::Waiting;
                Yield = true; // ic stays: re-evaluated on resume
                return SCRIPT_ERR_NONE;
            }
            Script->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_TIME_GET: {
            if (Ln.DestCount < 1) return SCRIPT_ERR_OPERAND;
            uint16_t Dtype = 0; uint8_t *Dest = nullptr; uint8_t Dsize = 0;
            if (!ResolveDest(Script, Script->Instr + (size_t)Ln.Start * 4, Dtype, Dest, Dsize)) return SCRIPT_ERR_OPERAND;
            // Time is a whole millisecond count. A Q16.16 Number destination only has a
            // 15-bit integer part (it overflows past ~32767 ms), so restrict Get time to
            // the integer types.
            if (Dtype != (uint16_t)DataType::Index && Dtype != (uint16_t)DataType::Uint32)
                return SCRIPT_ERR_TYPE;
            ScriptScalar Scalar;
            Scalar.Int = (int32_t)NowMs;
            Scalar.Num = Number::FromRaw(0); // unused: integer destination
            StoreScalar(Dtype, Dest, Dsize, false, Scalar);
            Script->Ic++;
            return SCRIPT_ERR_NONE;
        }
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}

// Reads a 4-byte BlockInfo from a Constant operand.
inline bool ScriptService::OperandBlockInfo(LoadedScript *Script, const uint8_t *Sym, uint32_t &Bi) {
    uint8_t Scratch[4];
    const uint8_t *Data = nullptr;
    uint8_t Size = 0;
    uint16_t Dtype = 0;
    if (!Resolve(Script, Sym[0], Sym[1], SymVal(Sym), Scratch, &Data, &Size, &Dtype)) return false;
    if (Size < 4) return false;
    memcpy(&Bi, Data, 4);
    return true;
}

inline void ScriptService::SendRegisterRequest(LoadedScript *Script, uint16_t Addr, uint8_t Cid,
                                      const uint8_t *Payload, uint16_t Len) {
    PacketConstruct(&TxFrame, Addr, MakeService(ServiceType::Register, Cid), Script->Trid,
                    FLAG_REQACK | FLAG_START | FLAG_STOP, Payload, Len);
    SendAndVerifyPacket(TxFrame);
}

// Writes an external-facing instruction's result code into its destination (0 = success). The
// code is a value like any other: a numeric destination converts it, another takes the raw bytes.
inline uint8_t ScriptService::StoreResultCode(LoadedScript *Script, const uint8_t *DestSym, uint8_t Code) {
    uint32_t CodeVal = Code;
    uint8_t Buf[4];
    memcpy(Buf, &CodeVal, 4);
    return Assign(Script, DestSym, Buf, 4, (uint16_t)DataType::Uint32);
}

inline uint8_t ScriptService::ExecService(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                                 uint8_t Op, uint32_t NowMs, bool &Yield) {
    switch (Op) {
        case SCRIPT_OP_SERVICE_NOP:
            Script->Ic++;
            return SCRIPT_ERR_NONE;
        case SCRIPT_OP_SERVICE_LOG: {
            // Custom log (docs "Custom logs"): the first operand's value becomes the
            // log code; the source is the Script service tagged with the instance.
            uint16_t Code = (uint16_t)(Script->Slot & 0xFF);
            if (Ln.OpCount >= 1) {
                int32_t Value = 0;
                if (ResolveOperandInt(Script, Opbase, Value)) Code = (uint16_t)(Value & 0xFFFF);
            }
            // A custom log is an informational message, not an error report.
            ReportLog(MakeLog(false, (uint16_t)ServiceType::Script, Code, 0), PRIORITY_LOG);
            Script->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_START:
            // Entering Running resets the instruction counter and the call depth, so a restart
            // resolves to Start. No advance: SetState may have reset the counter to 0.
            SetState(Script, (uint8_t)ScriptState::Running);
            return SCRIPT_ERR_NONE;
        case SCRIPT_OP_SERVICE_STOP:
            SetState(Script, (uint8_t)ScriptState::Stopped);
            return SCRIPT_ERR_NONE; // terminal: the run loop stops here
        case SCRIPT_OP_SERVICE_PAUSE:
            Script->Ic++; // resume continues past the Pause
            SetState(Script, (uint8_t)ScriptState::Paused);
            return SCRIPT_ERR_NONE;
        case SCRIPT_OP_SERVICE_RESUME:
            Script->Ic++;
            SetState(Script, (uint8_t)ScriptState::Running);
            return SCRIPT_ERR_NONE;
        case SCRIPT_OP_SERVICE_ERROR: {
            // A custom error throw: halt (state Error) and log the script-defined code. The code
            // is the script's own 32-bit value; the offending instruction is this Error op, so
            // the CID 3 report record names it via the single encoding helper.
            int32_t Code = 0;
            if (Ln.OpCount >= 1 && !ResolveOperandInt(Script, Opbase, Code)) return SCRIPT_ERR_OPERAND;
            SetFault(Script, ScriptInstructionEncoding(SCRIPT_CAT_SERVICE, SCRIPT_OP_SERVICE_ERROR),
                     (uint32_t)Code);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REPORT_INFO:
            // A log-only action: report the script's state and carry on (no halt).
            ReportLog(MakeLog(false, (uint16_t)ServiceType::Script, (uint32_t)Script->State, 0), PRIORITY_LOG);
            Script->Ic++;
            return SCRIPT_ERR_NONE;
        case SCRIPT_OP_SERVICE_REG_READ: { // dest = register[BlockInfo]
            if (Ln.DestCount < 1 || Ln.OpCount < 1) return SCRIPT_ERR_OPERAND;
            uint32_t Bi = 0;
            if (!OperandBlockInfo(Script, Opbase, Bi)) return SCRIPT_ERR_OPERAND;
            ValueInfo Rm;
            uint8_t Rbuf[256]; // a register value can be up to the u8 size limit
            uint8_t Rsz = 0;
            if (!RegisterGetByBlockInfo(Bi, Rm, Rbuf, Rsz)) return SCRIPT_ERR_REGISTER;
            uint8_t Err = Assign(Script, Script->Instr + (size_t)Ln.Start * 4, Rbuf, Rsz,
                                       ValueInfoType(Rm.Type));
            if (Err) return Err;
            Script->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_WRITE: { // register[BlockInfo] = operand2
            if (Ln.OpCount < 2) return SCRIPT_ERR_OPERAND;
            uint32_t Bi = 0;
            if (!OperandBlockInfo(Script, Opbase, Bi)) return SCRIPT_ERR_OPERAND;
            // Only the descriptor is needed here, so resolve without copying the current value
            // into a stack buffer (the read path still needs the copy).
            FieldResult Rf;
            if (!RegisterResolveByBlockInfo(Bi, Rf)) return SCRIPT_ERR_REGISTER;
            const ValueInfo Rm = Rf.Descriptor;
            uint8_t Scratch[4];
            const uint8_t *Val = nullptr;
            uint8_t Vsize = 0;
            uint16_t Vtype = 0;
            if (!Resolve(Script, Opbase[4], Opbase[5], SymVal(Opbase + 4), Scratch, &Val, &Vsize, &Vtype))
                return SCRIPT_ERR_OPERAND;
            uint8_t Wbuf[256];
            uint8_t Err = AssignResolved(ValueInfoType(Rm.Type), Wbuf, Rm.Size, Val, Vsize, Vtype);
            if (Err) return Err;
            ValueInfo Wm = Rm;
            if (!RegisterSetByBlockInfo(Bi, Wm, Wbuf, Rm.Size)) return SCRIPT_ERR_REGISTER;
            Script->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_READ_FOREIGN: { // dest = register[addr, BlockInfo]
            if (Ln.DestCount < 1 || Ln.OpCount < 2) return SCRIPT_ERR_OPERAND;
            int32_t Addr = 0;
            if (!ResolveOperandInt(Script, Opbase, Addr)) return SCRIPT_ERR_OPERAND;
            uint32_t Bi = 0;
            if (!OperandBlockInfo(Script, Opbase + 4, Bi)) return SCRIPT_ERR_OPERAND;
            memcpy(Script->PendingDest, Script->Instr + (size_t)Ln.Start * 4, 4);
            Script->PendingRead = true;
            Script->PendingForeign = true;
            Script->PendingTrid = Script->Trid;
            Script->PendingDeadline = NowMs + 500;
            Script->Ic++;
            Script->State = (uint8_t)ScriptState::Waiting;
            Script->WaitingOnTime = false;
            Yield = true;
            uint8_t Payload[4];
            memcpy(Payload, &Bi, 4);
            SendRegisterRequest(Script, (uint16_t)Addr, 1, Payload, 4);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_WRITE_FOREIGN: { // register[addr, BlockInfo] = operand3
            if (Ln.OpCount < 3) return SCRIPT_ERR_OPERAND;
            int32_t Addr = 0;
            if (!ResolveOperandInt(Script, Opbase, Addr)) return SCRIPT_ERR_OPERAND;
            uint32_t Bi = 0;
            if (!OperandBlockInfo(Script, Opbase + 4, Bi)) return SCRIPT_ERR_OPERAND;
            uint8_t Scratch[4];
            const uint8_t *Val = nullptr;
            uint8_t Vsize = 0;
            uint16_t Vtype = 0;
            if (!Resolve(Script, Opbase[8], Opbase[9], SymVal(Opbase + 8), Scratch, &Val, &Vsize, &Vtype))
                return SCRIPT_ERR_OPERAND;
            if (Vsize > 64) return SCRIPT_ERR_OPERAND; // like the local Reg-write path: refuse, never shorten
            uint8_t Payload[8 + 64];
            memcpy(Payload, &Bi, 4);
            ValueInfo Wm = { (uint16_t)Vtype, Vsize, 0 };
            memcpy(Payload + 4, &Wm, 4);
            if (Vsize) memcpy(Payload + 8, Val, Vsize);
            Script->PendingRead = false;
            Script->PendingForeign = true;
            Script->PendingTrid = Script->Trid;
            Script->PendingDeadline = NowMs + 500;
            Script->Ic++;
            Script->State = (uint8_t)ScriptState::Waiting;
            Script->WaitingOnTime = false;
            Yield = true;
            SendRegisterRequest(Script, (uint16_t)Addr, 2, Payload, (uint16_t)(8 + Vsize));
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_SCRIPT_LOAD: { // dest = load(script file id, loaded script id)
            // Mirrors management CID 1: the script picks the slot and the pair may differ. It is
            // external-facing, so it never faults - it carries its result code into a destination
            // (0 = success).
            if (Ln.DestCount < 1 || Ln.OpCount < 2) return SCRIPT_ERR_OPERAND;
            int32_t FileId = 0, Slot = 0;
            if (!ResolveOperandInt(Script, Opbase, FileId)) return SCRIPT_ERR_OPERAND;
            if (!ResolveOperandInt(Script, Opbase + 4, Slot)) return SCRIPT_ERR_OPERAND;
            // Loading over the running script would free the program currently being executed.
            uint8_t Code = SCRIPT_ERR_NONE;
            if (Slot == (int32_t)Script->Slot) Code = SCRIPT_ERR_OPERAND;
            else if (FileId < 0 || FileId >= MAX_SCRIPT_FILES || Slot < 0 || Slot >= MAX_SCRIPTS) Code = SCRIPT_ERR_BOUNDS;
            else if (!Load((uint16_t)FileId, (uint16_t)Slot)) Code = SCRIPT_ERR_BOUNDS;
            uint8_t Err = StoreResultCode(Script, Script->Instr + (size_t)Ln.Start * 4, Code);
            if (Err) return Err;
            Script->Ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_SCRIPT_UNLOAD: { // dest = unload(loaded script id)
            // Same operation as management CID 2 (which reports success or failure only).
            if (Ln.DestCount < 1 || Ln.OpCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t Slot = 0;
            if (!ResolveOperandInt(Script, Opbase, Slot)) return SCRIPT_ERR_OPERAND;
            // Never unload the running script: its program and spaces are what is executing.
            uint8_t Code = SCRIPT_ERR_NONE;
            if (Slot == (int32_t)Script->Slot) Code = SCRIPT_ERR_OPERAND;
            else if (Slot < 0 || Slot >= MAX_SCRIPTS || !Active((uint16_t)Slot)) Code = SCRIPT_ERR_BOUNDS;
            else Unload((uint16_t)Slot);
            uint8_t Err = StoreResultCode(Script, Script->Instr + (size_t)Ln.Start * 4, Code);
            if (Err) return Err;
            Script->Ic++;
            return SCRIPT_ERR_NONE;
        }
        default:
            return SCRIPT_ERR_NOT_IMPL;
    }
}

// Resolves one element of a vector/matrix/colour/string container.
inline bool ScriptService::ContainerElement(LoadedScript *Script, const uint8_t *Sym, int32_t Index,
                                   uint8_t *Scratch, const uint8_t **Elem, uint8_t *ElemSize,
                                   uint16_t *ElemType) {
    const uint8_t *Data = nullptr;
    uint8_t Size = 0;
    uint16_t Dtype = 0;
    if (!Resolve(Script, Sym[0], Sym[1], SymVal(Sym), Scratch, &Data, &Size, &Dtype)) return false;
    switch (Dtype) {
        case (uint16_t)DataType::Vector:
            // Test the magnitude before doing arithmetic on it: Index is a signed 32-bit operand, so
            // Index+1 can overflow and the x4 product can wrap, and either one lets a huge index
            // through to the pointer below.
            if (Index < 0 || (uint32_t)Index >= Size / 4) return false;
            *Elem = Data + (size_t)Index * 4;
            *ElemSize = 4;
            *ElemType = (uint16_t)DataType::Number;
            return true;
        case (uint16_t)DataType::Matrix: {
            if (Size < 4 || Index < 0) return false;
            uint16_t Rows = ScriptRd16(Data);
            uint16_t Cols = ScriptRd16(Data + 2);
            if ((uint32_t)Index >= (uint32_t)Rows * Cols) return false;
            if (4u + (uint32_t)(Index + 1) * 4 > Size) return false;
            *Elem = Data + 4 + (size_t)Index * 4;
            *ElemSize = 4;
            *ElemType = (uint16_t)DataType::Number;
            return true;
        }
        case (uint16_t)DataType::Colour:
            if (Index < 0 || Index > 3 || Size < 4) return false;
            *Elem = Data + Index;
            *ElemSize = 1;
            *ElemType = (uint16_t)DataType::Uint32;
            return true;
        case (uint16_t)DataType::Text:
            if (Index < 0 || Index >= Size) return false;
            *Elem = Data + Index;
            *ElemSize = 1;
            *ElemType = (uint16_t)DataType::Char;
            return true;
        default:
            if (Index != 0) return false;
            *Elem = Data;
            *ElemSize = Size;
            *ElemType = Dtype;
            return true;
    }
}

// Compose: container[index...] = value...   Extract: dest = container[index]
inline uint8_t ScriptService::ExecCompose(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op) {
    if (Op == SCRIPT_OP_COMPOSE) {
        if (Ln.DestCount < 1 || Ln.OpCount < 2) return SCRIPT_ERR_OPERAND;
        const uint8_t *DestSym = Script->Instr + (size_t)Ln.Start * 4;
        // The container must be writable (a variable or an output).
        if (DestSym[0] != SCRIPT_SYM_VARIABLE && DestSym[0] != SCRIPT_SYM_OUTPUT)
            return SCRIPT_ERR_OPERAND;
        int32_t Index = 0;
        if (!ResolveOperandInt(Script, Opbase, Index)) return SCRIPT_ERR_OPERAND;
        for (uint8_t k = 1; k < Ln.OpCount; k++) {
            uint8_t Vscratch[4];
            const uint8_t *Val = nullptr;
            uint8_t Vsize = 0;
            uint16_t Vtype = 0;
            if (!Resolve(Script, Opbase[k * 4], Opbase[k * 4 + 1], SymVal(Opbase + (size_t)k * 4),
                               Vscratch, &Val, &Vsize, &Vtype))
                return SCRIPT_ERR_OPERAND;
            const uint8_t *Elem = nullptr;
            uint8_t Esize = 0;
            uint16_t Etype = 0;
            // Separate scratch: `val` may point into `vscratch`.
            uint8_t Escratch[4];
            if (!ContainerElement(Script, DestSym, Index + (k - 1), Escratch, &Elem, &Esize, &Etype))
                return SCRIPT_ERR_BOUNDS;
            uint8_t Err = AssignResolved(Etype, const_cast<uint8_t *>(Elem), Esize, Val, Vsize, Vtype);
            if (Err) return Err;
        }
        Script->Ic++;
        return SCRIPT_ERR_NONE;
    }
    if (Op == SCRIPT_OP_EXTRACT) {
        if (Ln.DestCount < 1 || Ln.OpCount < 2) return SCRIPT_ERR_OPERAND;
        int32_t Index = 0;
        if (!ResolveOperandInt(Script, Opbase + 4, Index)) return SCRIPT_ERR_OPERAND;
        uint8_t Scratch[4];
        const uint8_t *Elem = nullptr;
        uint8_t Esize = 0;
        uint16_t Etype = 0;
        if (!ContainerElement(Script, Opbase, Index, Scratch, &Elem, &Esize, &Etype)) return SCRIPT_ERR_BOUNDS;
        uint8_t Err = Assign(Script, Script->Instr + (size_t)Ln.Start * 4, Elem, Esize, Etype);
        if (Err) return Err;
        Script->Ic++;
        return SCRIPT_ERR_NONE;
    }
    return SCRIPT_ERR_UNKNOWN_OP;
}

inline uint8_t ScriptService::ExecLine(LoadedScript *Script, uint32_t NowMs, bool &Yield) {
    const ScriptLineInfo &Ln = Script->Lines[Script->Ic];
    const uint8_t *Instr = Script->Instr + (size_t)(Ln.Start + Ln.DestCount) * 4;
    uint8_t Cat = Instr[1];
    uint8_t Op = Instr[2];
    const uint8_t *Opbase = Instr + 4;
    switch (Cat) {
        case SCRIPT_CAT_MATH:
        case SCRIPT_CAT_LOGIC:
            return ExecMath(Script, Ln, Opbase, Cat, Op);
        case SCRIPT_CAT_FLOW:
            return ExecFlow(Script, Ln, Opbase, Op);
        case SCRIPT_CAT_TIME:
            return ExecTime(Script, Ln, Opbase, Op, NowMs, Yield);
        case SCRIPT_CAT_SERVICE:
            return ExecService(Script, Ln, Opbase, Op, NowMs, Yield);
        case SCRIPT_CAT_COMPOSE:
            return ExecCompose(Script, Ln, Opbase, Op);
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}


#endif // USE_SCRIPTS
