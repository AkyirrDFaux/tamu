#pragma once

// Infix expression evaluation (the Set instruction) (ScriptExpr.h) - part of the script service.
//
// Scalar, vector and matrix expressions, element-wise with scalar broadcast.
//
// Split out of Script.h; included by it in order so the whole service stays one
// translation unit. Wrapped in the same guard so it is a no-op without USE_SCRIPTS.

#ifdef USE_SCRIPTS

#include "Core/Services/ScriptDefs.h"
#include "Core/Services/ScriptVm.h"
#include "Core/Types/Number.h"

// ---------------------------------------------------------------------------
// Infix expression evaluation (the Set instruction): scalar, vector and matrix
// values, element-wise with scalar broadcast (Docs/Services/Script.md: "generic math
// and logic processor"). Limit/Min/Max stay separate instructions.
// ---------------------------------------------------------------------------

#define SCRIPT_EXPR_MAX_ELEMS 9 // covers Number, Vector2/3, Matrix2x3, Colour
#define SCRIPT_EXPR_MAX_DEPTH 8 // paren/`^` nesting cap (keeps the VM stack bounded)

struct ExprValue
{
    uint16_t Dtype = (uint16_t)DataType::None;
    uint16_t Mh = 0, Mw = 0; // Matrix header (0 otherwise)
    uint8_t Count = 0;       // elements (1 = scalar)
    Number e[SCRIPT_EXPR_MAX_ELEMS];
};

struct ExprParser
{
    LoadedScript *s;
    const uint8_t *Tok;
    uint8_t n;
    uint8_t i;
    uint8_t Depth;
    uint8_t Scratch[4];
};

// Resolves a value token into an ExprValue (scalar / Vector / Matrix).
inline bool ScriptService::ExprResolve(ExprParser *p, const uint8_t *Sym, ExprValue &Out)
{
    const uint8_t *Data = nullptr;
    uint8_t Size = 0;
    uint16_t Dtype = 0;
    if (!Resolve(p->s, Sym[0], Sym[1], SymVal(Sym), p->Scratch, &Data, &Size, &Dtype))
        return false;
    Out.Dtype = Dtype;
    Out.Mh = 0;
    Out.Mw = 0;

    uint16_t Cnt = 1;
    if (Dtype == (uint16_t)DataType::Vector || Dtype == (uint16_t)DataType::Matrix)
    {
        Cnt = ContainerCount(Dtype, Size, Data);
        if (Cnt == 0 || Cnt > SCRIPT_EXPR_MAX_ELEMS) return false;
        if (Dtype == (uint16_t)DataType::Matrix)
        {
            Out.Mh = (uint16_t)(Data[0] | (Data[1] << 8));
            Out.Mw = (uint16_t)(Data[2] | (Data[3] << 8));
        }
    }
    for (uint16_t i = 0; i < Cnt; i++)
        if (!VectorElement(Data, Size, Dtype, i, Out.e[i])) return false;
    Out.Count = (uint8_t)Cnt;
    return true;
}

// `Math op` value at the cursor, or 0xFF when the next token is a value / end.
inline uint8_t ScriptService::ExprPeekOp(ExprParser *p)
{
    if (p->i >= p->n) return 0xFF;
    const uint8_t *Sym = p->Tok + (size_t)p->i * 4;
    if (Sym[0] == SCRIPT_SYM_PREDEFINE && Sym[1] == SCRIPT_PRE_MATHOP)
        return (uint8_t)(SymVal(Sym) & 0xFF);
    return 0xFF;
}

// base ^ exp. Fast paths: exp == 0.5 -> sqrt, integer exp -> repeated multiply (negative ->
// reciprocal). Any other exponent uses its binary expansion: with exp = int + sum(2^-k) over
// the set fraction bits, x^exp = x^int * prod(x^(2^-k)), and x^(2^-k) is k nested square
// roots. Fixed point end to end (no log/exp tables); the chain is only deepened while a lower
// fraction bit still needs it.
inline bool ScriptService::ExprPow(Number Base, Number Exp, Number &Out)
{
    if (Exp.Value == (int32_t)(1 << 15)) { Out = sqrt(Base); return true; } // 0.5

    bool Neg = Exp.Value < 0;
    if (Neg) Exp = -Exp;
    int32_t Ip = Exp.Value >> 16;      // integer part
    int32_t Frac = Exp.Value & 0xFFFF; // Q16.16 fraction
    if (Ip > 64) return false;
    if (Frac && Base.Value < 0) return false; // no real fractional power of a negative base

    Number r = Number(1);
    for (int32_t i = 0; i < Ip; i++) r = r * Base;
    if (Frac)
    {
        Number Chain = sqrt(Base); // base^(2^-1)
        int32_t Bit = 0x8000;
        while (Bit)
        {
            if (Frac & Bit) r = r * Chain;
            Bit >>= 1;
            // The chain must track the next bit's exponent; only skip a sqrt when no bit at
            // or below the new one is set (otherwise it would be one level too shallow).
            if (Bit && (Frac & ((Bit << 1) - 1))) Chain = sqrt(Chain);
        }
    }
    if (Neg) { if (r.Value == 0) return false; r = Number(1) / r; }
    Out = r;
    return true;
}

// Comparisons and logic ops (scalar operands -> Bool).
inline bool ScriptService::ExprIsBoolOp(uint8_t Op)
{
    return Op == SCRIPT_MATHOP_EQ || Op == SCRIPT_MATHOP_NE || Op == SCRIPT_MATHOP_LT ||
           Op == SCRIPT_MATHOP_LE || Op == SCRIPT_MATHOP_GT || Op == SCRIPT_MATHOP_GE ||
           Op == SCRIPT_MATHOP_AND || Op == SCRIPT_MATHOP_OR || Op == SCRIPT_MATHOP_XOR;
}

// a = a <op> b, element-wise (a scalar broadcasts to a container's shape). Comparisons and
// logic produce a scalar Bool and require scalar operands.
inline bool ScriptService::ExprApply(ExprValue &a, const ExprValue &b, uint8_t Op)
{
    if (ExprIsBoolOp(Op))
    {
        if (a.Count != 1 || b.Count != 1) return false;
        int32_t x = a.e[0].Value, y = b.e[0].Value;
        bool r;
        switch (Op)
        {
        case SCRIPT_MATHOP_EQ: r = (x == y); break;
        case SCRIPT_MATHOP_NE: r = (x != y); break;
        case SCRIPT_MATHOP_LT: r = (x < y); break;
        case SCRIPT_MATHOP_LE: r = (x <= y); break;
        case SCRIPT_MATHOP_GT: r = (x > y); break;
        case SCRIPT_MATHOP_GE: r = (x >= y); break;
        case SCRIPT_MATHOP_AND: r = (x != 0) && (y != 0); break;
        case SCRIPT_MATHOP_OR: r = (x != 0) || (y != 0); break;
        case SCRIPT_MATHOP_XOR: r = (x != 0) != (y != 0); break;
        default: return false;
        }
        a.Dtype = (uint16_t)DataType::Bool;
        a.Mh = 0;
        a.Mw = 0;
        a.Count = 1;
        a.e[0] = Number(r ? 1 : 0);
        return true;
    }

    uint8_t Count;
    if (a.Count == b.Count) Count = a.Count;
    else if (a.Count == 1) Count = b.Count;
    else if (b.Count == 1) Count = a.Count;
    else return false;
    const bool Abcast = (a.Count == 1);
    const Number Asrc = Abcast ? a.e[0] : Number(); // cached before the in-place writes
    if (Abcast && b.Count > 1) { a.Dtype = b.Dtype; a.Mh = b.Mh; a.Mw = b.Mw; }
    a.Count = Count;
    for (uint8_t k = 0; k < Count; k++)
    {
        Number Va = Abcast ? Asrc : a.e[k];
        Number Vb = (b.Count == 1) ? b.e[0] : b.e[k];
        switch (Op)
        {
        case SCRIPT_MATHOP_ADD: Va = Va + Vb; break;
        case SCRIPT_MATHOP_SUB: Va = Va - Vb; break;
        case SCRIPT_MATHOP_MUL: Va = Va * Vb; break;
        case SCRIPT_MATHOP_DIV: if (Vb.Value == 0) return false; Va = Va / Vb; break;
        case SCRIPT_MATHOP_MOD: {
            int32_t b = Vb.RoundToInt();
            if (b == 0) return false;
            Va = Number(Va.RoundToInt() % b);
            break;
        }
        case SCRIPT_MATHOP_POW: if (!ExprPow(Va, Vb, Va)) return false; break;
        default: return false;
        }
        a.e[k] = Va;
    }
    return true;
}

inline bool ScriptService::ExprPrimary(ExprParser *p, ExprValue &Out)
{
    if (p->Depth >= SCRIPT_EXPR_MAX_DEPTH) return false;
    p->Depth++;
    bool Ok;
    if (ExprPeekOp(p) == SCRIPT_MATHOP_OPEN)
    {
        p->i++;
        Ok = ExprBin(p, Out, 1);
        if (Ok)
        {
            if (ExprPeekOp(p) != SCRIPT_MATHOP_CLOSE) Ok = false;
            else p->i++;
        }
    }
    else if (p->i < p->n)
    {
        const uint8_t *Sym = p->Tok + (size_t)p->i * 4;
        Ok = ExprResolve(p, Sym, Out);
        if (Ok) p->i++;
    }
    else
    {
        Ok = false;
    }
    p->Depth--;
    return Ok;
}

// dot a b: sum(a_i * b_i) -> Number.
inline bool ScriptService::ExprFnDot(const ExprValue &a, const ExprValue &b, ExprValue &Out)
{
    if (a.Count == 0 || a.Count != b.Count) return false;
    Number s = Number(0);
    for (uint8_t k = 0; k < a.Count; k++) s = s + a.e[k] * b.e[k];
    Out.Dtype = (uint16_t)DataType::Number;
    Out.Mh = 0;
    Out.Mw = 0;
    Out.Count = 1;
    Out.e[0] = s;
    return true;
}// transpose m: Matrix R x C -> C x R.
inline bool ScriptService::ExprFnTranspose(const ExprValue &a, ExprValue &Out)
{
    if (a.Dtype != (uint16_t)DataType::Matrix || a.Mh == 0 || a.Mw == 0) return false;
    uint16_t R = a.Mh, C = a.Mw;
    if ((uint32_t)R * C > SCRIPT_EXPR_MAX_ELEMS) return false;
    Out.Dtype = (uint16_t)DataType::Matrix;
    Out.Mh = C;
    Out.Mw = R;
    Out.Count = a.Count;
    for (uint16_t r = 0; r < R; r++)
        for (uint16_t c = 0; c < C; c++)
            Out.e[c * R + r] = a.e[r * C + c];
    return true;
}// size v: Euclidean norm -> Number, i.e. sqrt(dot(v, v)).
inline bool ScriptService::ExprFnSize(const ExprValue &a, ExprValue &Out)
{
    // A resolved value always has at least one element, so dot's non-empty guard never
    // rejects a size().
    if (!ExprFnDot(a, a, Out)) return false;
    Out.e[0] = sqrt(Out.e[0]);
    return true;
}

// cross a b: Vector3 x Vector3 -> Vector3.
inline bool ScriptService::ExprFnCross(const ExprValue &a, const ExprValue &b, ExprValue &Out)
{
    if (a.Count != 3 || b.Count != 3) return false;
    Out.Dtype = (uint16_t)DataType::Vector;
    Out.Mh = 0;
    Out.Mw = 0;
    Out.Count = 3;
    Out.e[0] = a.e[1] * b.e[2] - a.e[2] * b.e[1];
    Out.e[1] = a.e[2] * b.e[0] - a.e[0] * b.e[2];
    Out.e[2] = a.e[0] * b.e[1] - a.e[1] * b.e[0];
    return true;
}

inline bool ScriptService::ExprUnary(ExprParser *p, ExprValue &Out)
{
    uint8_t Op = ExprPeekOp(p);
    if (Op == SCRIPT_MATHOP_FN_SIZE || Op == SCRIPT_MATHOP_FN_TRANSPOSE)
    {
        p->i++;
        ExprValue a;
        if (!ExprUnary(p, a)) return false;
        return (Op == SCRIPT_MATHOP_FN_SIZE) ? ExprFnSize(a, Out)
                                             : ExprFnTranspose(a, Out);
    }
    if (Op == SCRIPT_MATHOP_FN_DOT || Op == SCRIPT_MATHOP_FN_CROSS)
    {
        p->i++;
        ExprValue a, b;
        if (!ExprUnary(p, a)) return false;
        if (!ExprUnary(p, b)) return false;
        return (Op == SCRIPT_MATHOP_FN_DOT) ? ExprFnDot(a, b, Out)
                                            : ExprFnCross(a, b, Out);
    }
    if (Op == SCRIPT_MATHOP_SUB)
    {
        p->i++;
        ExprValue a;
        if (!ExprUnary(p, a)) return false;
        for (uint8_t k = 0; k < a.Count; k++) a.e[k] = -a.e[k];
        Out = a;
        return true;
    }
    if (Op == SCRIPT_MATHOP_NOT)
    {
        p->i++;
        ExprValue a;
        if (!ExprUnary(p, a)) return false;
        if (a.Count != 1) return false;
        Out.Dtype = (uint16_t)DataType::Bool;
        Out.Mh = 0;
        Out.Mw = 0;
        Out.Count = 1;
        Out.e[0] = Number(a.e[0].Value == 0 ? 1 : 0);
        return true;
    }
    return ExprPrimary(p, Out);
}

// Precedence climbing: MUL/DIV bind tighter than ADD/SUB; POW is right-associative.
inline bool ScriptService::ExprBin(ExprParser *p, ExprValue &Out, uint8_t minPrec)
{
    if (!ExprUnary(p, Out)) return false;
    for (;;)
    {
        uint8_t Op = ExprPeekOp(p);
        uint8_t Prec = (Op == SCRIPT_MATHOP_POW) ? 9
                       : (Op == SCRIPT_MATHOP_MUL || Op == SCRIPT_MATHOP_DIV ||
                          Op == SCRIPT_MATHOP_MOD) ? 8
                       : (Op == SCRIPT_MATHOP_ADD || Op == SCRIPT_MATHOP_SUB) ? 7
                       : (Op == SCRIPT_MATHOP_LT || Op == SCRIPT_MATHOP_LE ||
                          Op == SCRIPT_MATHOP_GT || Op == SCRIPT_MATHOP_GE) ? 5
                       : (Op == SCRIPT_MATHOP_EQ || Op == SCRIPT_MATHOP_NE) ? 4
                       : (Op == SCRIPT_MATHOP_AND) ? 3
                       : (Op == SCRIPT_MATHOP_XOR) ? 2
                       : (Op == SCRIPT_MATHOP_OR) ? 1 : 0;
        if (Prec == 0 || Prec < minPrec) break;
        p->i++;
        ExprValue b;
        uint8_t Next = (Op == SCRIPT_MATHOP_POW) ? Prec : (uint8_t)(Prec + 1);
        if (!ExprBin(p, b, Next)) return false;
        if (!ExprApply(Out, b, Op)) return false;
    }
    return true;
}

#endif // USE_SCRIPTS
